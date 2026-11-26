"""Route-room HPA1 PCM16 bank. Run under validation-lock.ps1 -Write.

HPA1 v2: <4s5I2f magic/version(2)/rate(32000)/channels/frames/stream/gain/pitch,
zero padding to byte 2048, then frames*channels little-endian int16 samples,
then zero padding to a 512-byte boundary. Loop points exclude all padding.
"""
import argparse
from array import array
import hashlib
import json
import math
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import wave

sys.path.insert(0, str(Path(__file__).resolve().parent))
import transcode_room as room

RATE = 32000
HEADER = struct.Struct('<4s5I2f')
PCM_OFFSET = 2048
PACK_HEADER = struct.Struct('<4s3I')
PACK_ENTRY = struct.Struct('<56s2I')
PACK_ALIGN = 2048


def scene_streams(catalog=None):
    catalog = catalog or room.REPO/'psp/include/room_audio_streams.inc'
    entries = []
    for line in catalog.read_text().splitlines():
        if not line or line.startswith('//'):
            continue
        match = re.fullmatch(r'ROOM_AUDIO_STREAM\((\d+), "([\w-]+\.pcm)", "([\w-]+\.bin)"\)', line)
        if not match:
            raise ValueError('Invalid room audio catalog entry')
        entries.append((int(match[1]), match[2], match[3]))
    if (not entries or len({x[0] for x in entries}) != len(entries) or
            len({x[1] for x in entries}) != len(entries) or len({x[2] for x in entries}) != len(entries) or
            any(not 240 <= x[0] < 256 for x in entries)):
        raise ValueError('Invalid/duplicate room audio stream')
    return entries


def pack_bank(output, names):
    """HPK1 v1: 16-byte header, 64-byte directory records, aligned HPA1 v2 blobs.

    Header: magic/version/count/total bytes. Record: NUL-padded ASCII filename,
    absolute blob offset/byte length. PCM stays byte-identical at blob+2048.
    No directory or extra PCM buffer is retained by the runtime.
    """
    if not 0 < len(names) <= 256 or len(set(names)) != len(names):
        raise ValueError('Invalid audio pack catalog')
    position = (PACK_HEADER.size+len(names)*PACK_ENTRY.size+PACK_ALIGN-1)//PACK_ALIGN*PACK_ALIGN
    directory = bytearray()
    for name in names:
        encoded = name.encode('ascii')
        if not encoded or len(encoded) >= 56 or Path(name).name != name:
            raise ValueError('Invalid audio pack name')
        size = (output/name).stat().st_size
        directory += PACK_ENTRY.pack(encoded, position, size)
        position = (position+size+PACK_ALIGN-1)//PACK_ALIGN*PACK_ALIGN
    if position >= 2**31:
        raise ValueError('Audio pack exceeds signed PSP file offsets')
    path = output/'audio.pak'
    with path.open('wb') as target:
        target.write(PACK_HEADER.pack(b'HPK1', 1, len(names), position))
        target.write(directory)
        for i, name in enumerate(names):
            _, offset, _ = PACK_ENTRY.unpack_from(directory, i*PACK_ENTRY.size)
            target.write(bytes(offset-target.tell()))
            target.write((output/name).read_bytes())
        target.write(bytes(position-target.tell()))
    return dict(file=path.name, format='HPK1', version=1, alignment=PACK_ALIGN,
                bytes=position, sha256=room.sha256(path))
SECTOR = 512
HERO = ('jump', 'softLanding', 'hardLanding', 'takeHit', 'footStepsRun',
        'normalSlash', 'alternateSlash', 'upSlash', 'downSlash', 'wallSlash', 'decor', 'plant')


def read_source(path):
    data = path.read_bytes()
    if data[:4] == b'HKAW':
        magic, rate, channels, frames = struct.unpack_from('<4s3I', data)
        pcm = data[16:]
    else:
        if len(data) < 44 or data[:4] != b'RIFF' or data[8:12] != b'WAVE':
            raise ValueError(f'Not PCM WAV: {path}')
        if struct.unpack_from('<I', data, 4)[0] != len(data)-8:
            raise ValueError('WAV exact size')
        with wave.open(str(path), 'rb') as f:
            if f.getsampwidth() != 2 or f.getcomptype() != 'NONE':
                raise ValueError('Expected PCM16')
            rate, channels, frames = f.getframerate(), f.getnchannels(), f.getnframes()
            pcm = f.readframes(frames)
    if rate not in (16000, 22050, 32000, 44100, 48000) or channels not in (1, 2) or frames <= 0 or len(pcm) != frames*channels*2:
        raise ValueError(f'Invalid PCM source: {path}')
    return rate, channels, frames, pcm


def resample(pcm, rate, channels, ffmpeg, overlay=None):
    # Float output exposes intersample overshoot before quantization. libsoxr's
    # 28-bit precision band-limited resampler is deterministic for this tool build.
    cmd = [str(ffmpeg), '-v', 'error', '-f', 's16le', '-ar', str(rate), '-ac', str(channels),
           '-i', 'pipe:0']
    resampler = 'aresample=32000:resampler=soxr:precision=28'
    if overlay is not None:
        path, gain = overlay
        overlay_rate, overlay_channels, frames, _ = read_source(path)
        if ((overlay_rate, overlay_channels, frames) != (rate, channels, len(pcm)//(channels*2)) or
                path.suffix != '.wav' or not math.isfinite(gain) or not 0 < gain <= 1):
            raise ValueError('Premix requires equal-rate/channel/length synchronized WAV and finite gain')
        # Mix as float before the sole resample/PCM16 quantization. No amix
        # normalization: -60 dB must not reduce the original Main stem's level.
        cmd += ['-i', str(path), '-filter_complex',
                f'[0:a][1:a]amix=inputs=2:weights=1 {gain:.17g}:normalize=0:duration=first,'+resampler]
    else:
        cmd += ['-af', resampler]
    cmd += ['-f', 'f32le', '-acodec', 'pcm_f32le', 'pipe:1']
    result = subprocess.run(cmd, input=pcm, capture_output=True, check=True)
    values = array('f'); values.frombytes(result.stdout)
    if sys.byteorder != 'little':
        values.byteswap()
    if not values or not all(math.isfinite(v) for v in values):
        raise ValueError('Nonfinite/empty resampler output')
    peak = max(abs(v) for v in values)
    scale = min(1., (32767/32768)/peak) if peak else 1.
    # Round half away from zero, no dither and no saturating clamp hiding clips.
    samples = array('h', (math.floor(v*scale*32768+.5) if v >= 0 else
                          math.ceil(v*scale*32768-.5) for v in values))
    if len(samples) % channels:
        raise ValueError('Partial resampled frame')
    quantized_peak = max(abs(v) for v in samples)
    if quantized_peak > 32767:
        raise ValueError('Quantization clipped')
    if sys.byteorder != 'little':
        samples.byteswap()
    return samples.tobytes(), peak, scale, quantized_peak


def encode(channels, frames, stream, gain, pitch, pcm):
    if len(pcm) != frames*channels*2:
        raise ValueError('Invalid PCM size')
    data = HEADER.pack(b'HPA1', 2, RATE, channels, frames, stream, gain, pitch)
    data += bytes(PCM_OFFSET-HEADER.size)+pcm
    return data+bytes(-len(data) % SECTOR)


def decode(data):
    if len(data) < HEADER.size:
        raise ValueError('Truncated HPA1')
    magic, version, rate, channels, frames, stream, gain, pitch = HEADER.unpack_from(data)
    # Original Crossroads music lasts 153.6 s; streamed PCM uses fixed rings.
    if (magic, version, rate) != (b'HPA1', 2, RATE) or channels not in (1, 2) or stream not in (0, 1) or not 0 < frames <= RATE*(180 if stream else 120):
        raise ValueError('Invalid HPA1 header')
    if stream and channels != 2 or not all(math.isfinite(v) and 0 <= v <= 4 for v in (gain, pitch)) or pitch == 0:
        raise ValueError('Invalid HPA1 settings')
    end = PCM_OFFSET+frames*channels*2
    if len(data) != (end+SECTOR-1)//SECTOR*SECTOR:
        raise ValueError('Invalid HPA1 exact size')
    if any(data[HEADER.size:PCM_OFFSET]) or any(data[end:]):
        raise ValueError('Nonzero HPA1 padding')
    return channels, frames, stream, gain, pitch, data[PCM_OFFSET:end]


def resident_frames(frames, stream):
    # Runtime AudioClip::load policy; one extra frame preserves interpolation.
    return 0 if stream else 8001 if frames > RATE//2 else frames


def convert(source, output, ffmpeg=None):
    ffmpeg = ffmpeg or shutil.which('ffmpeg')
    if not ffmpeg:
        raise ValueError('ffmpeg with libsoxr is required')
    root = source/'_private/converted/hero/romfs'
    settings = root/'sfx-settings.bin'
    raw = settings.read_bytes()
    if len(raw) != 80:
        raise ValueError('Expected 20 hero settings')
    values = list(struct.unpack('<20f', raw)) + [1.]*4
    if not all(math.isfinite(v) and 0 <= v <= 4 for v in values) or any(values[i] == 0 for i in range(1, 24, 2)):
        raise ValueError('Invalid source hero gain/pitch')
    entries = [(f'audio-h{i}.pcm', root/f'sfx-{name}.wav', 0, values[2*i], values[2*i+1]) for i, name in enumerate(HERO)]
    catalog = room.REPO/'psp/include/audio_assets.inc'
    keys = set()
    movement = {'Tiktik': 'sharedassets37.assets:30', 'Gruzzer': 'sharedassets33.assets:22',
                'HuskFootstep': 'sharedassets32.assets:63', 'Hatchling': 'sharedassets57.assets:11',
                'Hatcher': 'sharedassets57.assets:12', 'CityTollActivate': 'sharedassets9.assets:52',
                'CityLiftActivate': 'sharedassets79.assets:5', 'CityLiftArrive': 'sharedassets79.assets:6',
                'CityLiftClose': 'sharedassets79.assets:7', 'CityLiftOpen': 'sharedassets7.assets:112',
                'JijiKeyTurn': 'sharedassets7.assets:108', 'JijiOpenAntic': 'sharedassets7.assets:107',
                'JijiOpenSlamEnd': 'sharedassets7.assets:98'}
    # These original clips are absent from the upstream WAV export.
    extractor = Path(__file__).with_name('extract_movement_audio.py')
    # The extractors need UnityPy: run them under this converter's own Python
    # (HKPSP_PYTHON from regen-assets.ps1), never a launcher's default (2026-11-25:
    # `py -3.12` had no UnityPy in a clean venv build).
    interpreter = [sys.executable]
    subprocess.run([*interpreter, '-I', str(extractor), '--source', str(source),
                    '--out', str(output/'source-wav')], check=True)
    # Keep Main and synchronized Action independent: snapshots address their
    # mixer groups separately. Normal's Action gain belongs in its own header.
    subprocess.run([*interpreter, '-I', str(Path(__file__).with_name('extract_room_music.py')),
                    '--source', str(source), '--out', str(output/'source-wav')], check=True)
    music = json.loads((output/'source-wav/room-music.json').read_text())
    subprocess.run([*interpreter, '-I', str(Path(__file__).with_name('extract_room_atmosphere.py')),
                    '--source', str(source), '--out', str(output/'source-wav')], check=True)
    atmosphere = json.loads((output/'source-wav/room-atmosphere.json').read_text())
    local_streams = {clip['file'].replace('.wav', '.bin'): clip for clip in music['clips']}
    local_streams.update({f"wind-{clip['name']}.bin": clip for clip in atmosphere['clips']})
    if read_source(root/'music-crossroads-main.bin') != read_source(output/'source-wav/music-crossroads-main.wav'):
        raise ValueError('Converted Crossroads Main differs from the admitted original decode')
    main_format = read_source(output/'source-wav/music-crossroads-main.wav')[:3]
    if read_source(output/'source-wav/music-crossroads-action.wav')[:3] != main_format:
        raise ValueError('Synchronized Main/Action require equal rate, channels and loop length')
    for line in catalog.read_text().splitlines():
        if not line or line.startswith('//'):
            continue
        match = re.fullmatch(r'AUDIO_ASSET\((\w+), "((?:[\w-]+/)*[\w-]+)"\)', line)
        if not match or match[1] in keys:
            raise ValueError('Invalid/duplicate Tutorial audio catalog entry')
        key, name = match.groups();keys.add(key)
        if key.startswith('Knight'):
            path = source/'_private/converted/false-knight-audio/source'/f'{name}.wav'
        elif key.startswith('Uumuu'):
            path = source/'_private/converted/uumuu-audio'/f'{name}.wav'
        else:
            path = output/'source-wav'/f'{name}.wav' if key in movement else root/f'{name}.wav'
        entries.append((f'audio-{key}.pcm', path, 0, 1., 1.))
    if len(keys) != 115:
        raise ValueError('PSP audio catalog must contain 115 cues')
    streams = scene_streams()
    for _, name, original in streams:
        local = local_streams.get(original)
        path = output/'source-wav'/local['file'] if local else root/original
        entries.append((name, path, 1, 10**(local['gain_db']/20) if local else 1., 1.))
    output.mkdir(parents=True, exist_ok=True)
    rows = []
    for name, path, stream, gain, pitch in entries:
        rate, channels, frames, pcm = read_source(path)
        converted, peak, scale, quantized_peak = resample(pcm, rate, channels, ffmpeg)
        frames_out = len(converted)//(2*channels)
        if abs(frames_out-frames*RATE/rate) > 1:
            raise ValueError('Resampler duration changed')
        data = encode(channels, frames_out, stream, gain, pitch, converted)
        decode(data)
        (output/name).write_bytes(data)
        origin = movement.get(name.removeprefix('audio-').removesuffix('.pcm'))
        original_clip = next((clip for clip in local_streams.values()
                              if path == output/'source-wav'/clip['file']), None)
        if original_clip:
            origin = f"{original_clip['collection']}:AudioClip:{original_clip['path_id']}"
        rows.append(dict(file=name, source=origin or str(path.relative_to(source)), source_sha256=room.sha256(path),
                         sha256=hashlib.sha256(data).hexdigest(), rate_in=rate, frames_in=frames,
                         channels=channels, frames=frames_out, stream=bool(stream), bytes=len(data),
                         resident_frames=resident_frames(frames_out, stream),
                         tail_stream=not stream and frames_out > RATE//2,
                         float_peak=peak, attenuation=scale, pcm_peak=quantized_peak, gain=gain, pitch=pitch))
        if original_clip:
            rows[-1]['original'] = original_clip
    pack = pack_bank(output, [r['file'] for r in rows])
    manifest = dict(format='HPA1', version=2, pcm_offset=PCM_OFFSET, sector_bytes=SECTOR, pack=pack,
                    rate=RATE, resampler='libsoxr precision=28; float -> PCM16; no dither',
                    ffmpeg_version=subprocess.check_output([str(ffmpeg), '-version'], text=True).splitlines()[0],
                    settings_sha256=room.sha256(settings), catalog_sha256=room.sha256(catalog), clips=rows,
                    scene_streams=[dict(clip=clip, file=name, source=source) for clip, name, source in streams],
                    stream_bytes_per_second=8*RATE*2*2,
                    resident_bytes=sum(r['resident_frames']*r['channels']*2 for r in rows),
                    tail_buffer_bytes=29*3*4096*2*2,
                    stream_prefix_bytes=len(streams)*8000*2*2,
                    ambience_buffer_bytes=len(streams)*(32768+8000)*2*2)
    (output/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--source', type=Path, default=room.source_root())
    parser.add_argument('--out', type=Path, default=room.REPO/'_private/converted-psp/audio')
    parser.add_argument('--ffmpeg', type=Path)
    args = parser.parse_args()
    m = convert(args.source, args.out, args.ffmpeg)
    print(f"Audio: {len(m['clips'])} clips, {m['resident_bytes']} resident bytes, {m['stream_bytes_per_second']} stream B/s")
