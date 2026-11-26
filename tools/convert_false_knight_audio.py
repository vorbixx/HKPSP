"""Package preserved False Knight native PCM; --extract reads copied originals."""
import converted_files
import argparse
import hashlib
import json
import io
import struct
import sys
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / '_private/converted/false-knight-audio/source'
CLIPS = (
    (46, 19, 'false_knight_damage_armour'),
    (48, 24, 'false_knight_head_damage_2'),
    (48, 45, 'false_knight_jump'),
    (48, 28, 'false_knight_strike_ground'),
    (48, 43, 'Fknight_daze'),
    (48, 30, 'FKnight_Rage'),
    (48, 38, 'FKnight_death'),
    (48, 39, 'false_knight_swing'),
    (48, 19, 'Fknight_flump_01'),
    (48, 42, 'Boss Battle 1'),
)


def extract(source, packages, game_data=ROOT / '_private/original/Hollow Knight/hollow_knight_Data'):
    sys.path.insert(0, str(packages))
    import UnityPy
    source.mkdir(parents=True, exist_ok=True)
    files, hashes = {}, {}
    for collection, path_id, name in CLIPS:
        if collection not in files:
            path = game_data / f'sharedassets{collection}.assets'
            files[collection] = next(iter(UnityPy.load(str(path)).files.values()))
            hashes[path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
            resource = path.with_suffix('.resource')
            hashes[resource.name] = hashlib.sha256(resource.read_bytes()).hexdigest()
        clip = files[collection].objects[path_id].read()
        if clip.m_Name != name or len(clip.samples) != 1:
            raise ValueError(f'Unexpected source {collection}:{path_id}')
        (source / f'{name}.wav').write_bytes(next(iter(clip.samples.values())))
    (source / 'source-hashes.json').write_text(json.dumps(hashes, indent=2)+'\n')


def convert(source, output):
    output.mkdir(parents=True, exist_ok=True)
    entries = []
    for collection, path_id, name in CLIPS:
        path = source / f'{name}.wav'
        data = path.read_bytes()
        with wave.open(str(path), 'rb') as wav:
            channels, rate, frames = wav.getnchannels(), wav.getframerate(), wav.getnframes()
            if wav.getsampwidth() != 2 or channels not in (1, 2) or rate != 44100:
                raise ValueError(f'Unexpected native PCM format: {name}')
            pcm = wav.readframes(frames)
        if channels == 1:
            # Existing bounded one-shot streamer consumes stereo PCM16.
            pcm = b''.join(pcm[i:i+2]*2 for i in range(0, len(pcm), 2))
            channels = 2
            encoded = io.BytesIO()
            with wave.open(encoded, 'wb') as wav:
                wav.setparams((2, 2, rate, frames, 'NONE', 'not compressed'))
                wav.writeframes(pcm)
            data = encoded.getvalue()
        filename = f'sfx-enemy-{name}.wav'
        if name == 'Boss Battle 1':
            filename = 'music-false-knight.bin'
            data = struct.pack('<4sIII', b'HKAW', rate, channels, frames)+pcm
        (output / filename).write_bytes(data)
        entries.append(dict(collection=collection, path_id=path_id, name=name, output=filename,
                            channels=channels, rate=rate, frames=frames, sha256=hashlib.sha256(data).hexdigest()))
    manifest = dict(resident_pcm_bytes=0, clips=entries,
                    source_hashes=json.loads((source/'source-hashes.json').read_text()))
    (output/'false-knight-audio-manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    print(f'False Knight audio: {len(entries)} native streams; no additional resident PCM')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=SOURCE)
    parser.add_argument('--output', type=Path, default=ROOT/'_private/converted/hero/romfs')
    parser.add_argument('--packages', type=Path, default=ROOT/'_private/tools/audio-python')
    parser.add_argument('--extract', action='store_true')
    args = parser.parse_args()
    if args.extract:
        extract(args.source, args.packages)
    convert(args.source, args.output)
