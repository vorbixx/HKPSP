"""Export/convert supported room atmosphere at its native stereo rates.

Offline PCM decoding uses bounded 44100/48000 Hz runtime streams;
no resampling, downmixing, crossfade, inserted silence or resident full wind bank.
Initial chunks are cached in 48 KiB of ordinary heap for scene starts/retries.
"""
import converted_files  # Install hardlink copy-on-write before asset writes.
import argparse
import hashlib
import json
import struct
import subprocess
from pathlib import Path

try:
    from .convert_enemy_audio import FFMPEG
except ImportError:
    from convert_enemy_audio import FFMPEG

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / '_private/converted/wind/source'
OUTPUT = ROOT / '_private/converted/hero/romfs'
CLIPS = ((1151, 'cave_wind_loop'), (1257, 'dirtmouth_wind_loop_a'),
         (1323, 'dirtmouth_wind_loop_b_grass'), (1292, 'green_path_atmos_loop'),
         (1345, 'cave_noises'))
NATIVE_RATES = {1151: 44100, 1257: 44100, 1323: 44100, 1292: 48000, 1345: 48000}
ASSET_SHA256 = '6d49ac5a50a282a645acc32518b28e7a29d0b94a0c3b5a4cda18dade3e6fe511'


def source_path(directory, path_id, name):
    return directory / f'resources-assets-{path_id}-{name}.ogg'


def extract_sources(directory=SOURCE, port=58291, *, client=None):
    from assetripper_client import Client
    directory.mkdir(parents=True, exist_ok=True)
    client = client or Client(port)
    for path_id, name in CLIPS:
        path = source_path(directory, path_id, name)
        if path.exists():
            continue
        rows = [r for r in client.search(name) if r['type'] == 'AudioClip'
                and r['collection'] == 'resources.assets' and r['path']['D'] == path_id]
        if len(rows) != 1:
            raise ValueError(f'Expected exact resources.assets:{path_id} {name}')
        path.write_bytes(client.request('/Assets/Audio', Path=json.dumps(rows[0]['path'], separators=(',', ':'))))


def convert(source=SOURCE, output=OUTPUT, decoder=FFMPEG):
    if decoder == FFMPEG and not decoder.is_file():
        import shutil
        decoder = shutil.which('ffmpeg') or decoder
    prepared = []
    for path_id, name in CLIPS:
        encoded = source_path(source, path_id, name).read_bytes()
        pos = encoded.find(b'\x01vorbis', 0, 512)
        if not encoded.startswith(b'OggS') or pos < 0 or len(encoded) < pos+16:
            raise ValueError(f'Invalid Vorbis export for {name}')
        channels, rate = encoded[pos+11], struct.unpack_from('<I', encoded, pos+12)[0]
        if (channels, rate) != (2, NATIVE_RATES[path_id]):
            raise ValueError(f'Unexpected native format for {name}; quality change requires review')
        decoded = subprocess.run([str(decoder), '-v', 'error', '-i', 'pipe:0', '-f', 's16le',
                                  '-acodec', 'pcm_s16le', 'pipe:1'], input=encoded,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        if decoded.returncode or not decoded.stdout or len(decoded.stdout) % 4:
            raise ValueError(f'Invalid native PCM decode for {name}: {decoded.stderr.decode(errors="replace")}')
        pcm = decoded.stdout
        data = struct.pack('<4sIII', b'HKAW', rate, channels, len(pcm)//4) + pcm
        prepared.append((f'wind-{name}.bin', data, dict(
            collection='resources.assets', path_id=path_id, name=name,
            channels=channels, sample_rate=rate, frames=len(pcm)//4, pcm16_bytes=len(pcm),
            gain=1, pitch=1, source_ogg_sha256=hashlib.sha256(encoded).hexdigest(),
            output_sha256=hashlib.sha256(data).hexdigest())))
    output.mkdir(parents=True, exist_ok=True)
    for filename, data, entry in prepared:
        (output/filename).write_bytes(data)
        entry['output'] = filename
    manifest = dict(format='HKAW PCM16 stereo; native rate per clip', source_asset_sha256=ASSET_SHA256,
                    scene='Tutorial_01', cue='sharedassets6.assets:1054',
                    enabled_channels=[0, 4, 5, 6], admitted_channels=[0, 4],
                    omitted_channels=[5, 6], omitted_mixer_db=[-60.40684, -60.40684],
                    cue_approximation='Omit quiet rain and retained music under Silent (-60 dB)',
                    source_gain_before_mixer=True, mixer_snapshot='resources.assets:3852',
                    mixer_snapshot_name='at Cave', mixer_snapshot_db=[0, 0],
                    mixer_groups=[3841, 3835], music_cue=None,
                    music_snapshot='resources.assets:3896',
                    additional_cues=[dict(cue='sharedassets128.assets:174', enabled_channels=[7],
                        clip='resources.assets:1292', mixer_snapshot='resources.assets:3864',
                        mixer_snapshot_name='at Greenpath', mixer_group=3820, mixer_db=0,
                        source_gain=1, source_pitch=1, loop=True),
                        dict(scene='Crossroads_ShamanTemple', cue='sharedassets6.assets:1054',
                            enabled_channels=[0, 4, 5, 6], admitted_channels=[0, 4],
                            clips=['resources.assets:1151', 'resources.assets:1345'],
                            mixer_snapshot='resources.assets:3852', mixer_snapshot_name='at Cave',
                            mixer_groups=[3841, 3835], mixer_db=[0, 0],
                            source_gain=1, source_pitch=1, loop=True,
                            omitted_channels=[5, 6], omitted_mixer_db=[-60.40684, -60.40684],
                            music_cue=None, music_snapshot='resources.assets:3896',
                            approximation='Omit quiet rain and retained music under Silent (-60 dB)')],
                    pcm_start_cache_bytes=3*16*1024,
                    transition_seconds=0.5, pcm_resident_bytes=0,
                    pcm_streamed_bytes=sum(len(data)-16 for _, data, _ in prepared),
                    clips=[entry for _, _, entry in prepared])
    (output/'wind-audio-manifest.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=SOURCE)
    parser.add_argument('--output', type=Path, default=OUTPUT)
    parser.add_argument('--decoder', type=Path, default=FFMPEG)
    parser.add_argument('--extract', action='store_true')
    parser.add_argument('--port', type=int, default=58291)
    args = parser.parse_args()
    if args.extract:
        extract_sources(args.source, args.port)
    manifest = convert(args.source, args.output, args.decoder)
    print(f"Wind: {len(manifest['clips'])} native stereo streams, {manifest['pcm_streamed_bytes']} PCM bytes in RomFS")
