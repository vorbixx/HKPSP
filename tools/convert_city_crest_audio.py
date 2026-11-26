"""Preserve the three native City Crest WAVs in the scoped scene package."""
import argparse
import hashlib
import json
from pathlib import Path
import wave

import converted_files
from assetripper_client import ROOT
from private_output_paths import is_private_output

SOURCE = ROOT / '_private/minimum-obstacles/city-crest-audio'
CLIPS = ((107, 'open', 'jiji_door_open_pt_1'), (100, 'move', 'lift_moving_loop'),
         (98, 'bump', 'jiji_door_open_pt_2'))
CAP = 1200000


def convert(source, output):
    if not is_private_output(output):
        raise ValueError('City Crest audio output must remain private')
    report = json.loads((source / 'source-report.json').read_text())
    entries = {c['role']: c for c in report['clips']}
    samples = []
    total = 0
    for ident, role, name in CLIPS:
        entry = entries[role]
        raw = (source / f'{role}.wav').read_bytes()
        if (entry['collection'], entry['path_id'], entry['name']) != (7, ident, name) or \
                hashlib.sha256(raw).hexdigest() != entry['sha256']:
            raise ValueError('Changed City Crest source AudioClip')
        with wave.open(str(source / f'{role}.wav')) as wav:
            if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate(), wav.getcomptype()) != (2, 2, 44100, 'NONE'):
                raise ValueError('Unsupported City Crest native PCM')
            size = wav.getnframes() * 4
            if len(wav.readframes(wav.getnframes())) != size:
                raise ValueError('Truncated City Crest PCM')
            total += size
        samples.append((f'city-crest-{role}.wav', raw))
    if total > CAP:
        raise ValueError('City Crest PCM exceeds measured scene budget')
    output.mkdir(parents=True, exist_ok=True)
    for name, raw in samples:
        (output / name).write_bytes(raw)
    print(f'City Crest audio: {len(samples)} original native clips, {total} resident PCM bytes')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=SOURCE)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    convert(args.source, args.output)
