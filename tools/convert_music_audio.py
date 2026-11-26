"""Export copied-original area clips and package native PCM for the wind streamer.

Normal conversion is offline. --extract requires the existing private UnityPy
packages and reads only the preserved original copy, never the Steam install.
"""
import converted_files  # Install hardlink copy-on-write before asset writes.
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
import wave

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / '_private/converted/music/source'
OUTPUT = ROOT / '_private/converted/hero/romfs'
CLIPS = (
    ('sharedassets7.assets', 93, 'Dirtmouth 1', 'music-dirtmouth'),
    ('sharedassets19.assets', 30, 'S19 Crossroads Bass', 'music-crossroads-bass'),
    ('sharedassets19.assets', 27, 'S19 Crossroads Main', 'music-crossroads-main'),
    ('resources.assets', 1234, 'dirtmouth_wind_loop_c_gate', 'wind-dirtmouth_wind_loop_c_gate'),
    ('sharedassets36.assets', 14, 'S5 Green Path Bass', 'music-greenpath-bass'),
    ('sharedassets36.assets', 13, 'S5 Green Path Main', 'music-greenpath-main'),
)


def extract_sources(source=SOURCE, original=None, packages=None):
    original = original or ROOT / '_private/original/Hollow Knight/hollow_knight_Data'
    # These are already installed project tools, not downloaded dependencies.
    for package in (packages, ROOT / '_private/tools/audio-python', ROOT / '_private/tools/boss-python'):
        if package:
            sys.path.insert(0, str(package))
    import UnityPy
    source.mkdir(parents=True, exist_ok=True)
    files, hashes = {}, {}
    for collection, path_id, name, output in CLIPS:
        if collection not in files:
            path = original / collection
            files[collection] = next(iter(UnityPy.load(str(path)).files.values()))
            for p in (path, path.with_suffix('.resource')):
                hashes[p.name] = hashlib.sha256(p.read_bytes()).hexdigest()
        clip = files[collection].objects[path_id].read()
        if clip.m_Name != name:
            raise ValueError(f'Unexpected original clip at {collection}:{path_id}')
        samples = clip.samples
        if len(samples) != 1:
            raise ValueError(f'Expected one audio sample for {name}')
        (source / f'{output}.wav').write_bytes(next(iter(samples.values())))
    manifest = ROOT / '_private/original/appmanifest_367520.acf'
    if manifest.exists():
        hashes[manifest.name] = hashlib.sha256(manifest.read_bytes()).hexdigest()
    (source / 'source-hashes.json').write_text(json.dumps(hashes, indent=2)+'\n')


def convert(source=SOURCE, output=OUTPUT):
    prepared = []
    for collection, path_id, name, stem in CLIPS:
        path = source / f'{stem}.wav'
        with wave.open(str(path), 'rb') as wav:
            if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate(), wav.getcomptype()) != (2, 2, 44100, 'NONE'):
                raise ValueError(f'Expected native stereo PCM16 44100 Hz: {name}')
            frames = wav.getnframes()
            pcm = wav.readframes(frames)
        if not frames or len(pcm) != frames*4:
            raise ValueError(f'Incomplete PCM for {name}')
        data = struct.pack('<4sIII', b'HKAW', 44100, 2, frames)+pcm
        prepared.append((stem+'.bin', data, dict(collection=collection, path_id=path_id,
            name=name, channels=2, sample_rate=44100, frames=frames,
            source_wav_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
            output_sha256=hashlib.sha256(data).hexdigest())))
    output.mkdir(parents=True, exist_ok=True)
    for filename, data, entry in prepared:
        (output / filename).write_bytes(data)
        entry['output'] = filename
    provenance = source / 'source-hashes.json'
    manifest = dict(format='HKAW PCM16 stereo 44100 Hz',
        source_hashes=json.loads(provenance.read_text()) if provenance.exists() else {},
        pcm_resident_bytes=0, pcm_streamed_bytes=sum(len(data)-16 for _, data, _ in prepared),
        clips=[entry for _, _, entry in prepared],
        approximations=['Fixed exploration stems; no action/tension snapshots, infected or accordion alternatives.',
                        'Existing 0.5 s source fade replaces original music delay/snapshot transitions.'])
    (output / 'music-audio-manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=SOURCE)
    parser.add_argument('--output', type=Path, default=OUTPUT)
    parser.add_argument('--extract', action='store_true')
    parser.add_argument('--original', type=Path)
    parser.add_argument('--packages', type=Path)
    args = parser.parse_args()
    if args.extract:
        extract_sources(args.source, args.original, args.packages)
    result = convert(args.source, args.output)
    print(f"Area audio: {len(result['clips'])} streams, {result['pcm_streamed_bytes']} PCM bytes")
