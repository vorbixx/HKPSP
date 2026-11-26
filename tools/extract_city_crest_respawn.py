"""Recover the City front gate's authored death marker in an isolated session.

Run through validation-lock.ps1 -Exclusive. Original files remain immutable.
"""
import argparse
import hashlib
import json
from pathlib import Path

from assetripper_client import ROOT
from extract_crawler import Export
from extract_world_switch_session import run


def extract(client, source, output):
    matches = [r for r in client.search('Death Respawn Marker')
               if r['type'] == 'GameObject' and r['name'] == 'Death Respawn Marker' and r['collection'] == 'level186']
    if len(matches) != 1:
        raise ValueError(f'Expected one Fungus2_21 death marker, found {len(matches)}')
    output.mkdir(parents=True, exist_ok=True)
    collection = matches[0]['path']['C']
    exporter = Export(client, output)
    parts = exporter.components(collection, matches[0]['path']['D'])
    transforms = list(parts.get('Transform', []))
    if len(transforms) != 1:
        raise ValueError('Missing/ambiguous City death marker transform')
    transform = exporter.cache[transforms[0]]
    while transform['m_Father']['m_PathID']:
        transform, name, collection = exporter.resolve(collection, transform['m_Father'])
        if name in transforms or len(transforms) >= 64:
            raise ValueError('Cyclic City death marker hierarchy')
        transforms.append(name)
    original = ROOT / '_private/original/Hollow Knight/hollow_knight_Data/level186'
    report = dict(scene='Fungus2_21', name='Death Respawn Marker', spawnType=0, facingRight=True,
                  source='level186-5613.json / Slam / SetBenchRespawn', components=parts, transforms=transforms,
                  level_sha256=hashlib.sha256(original.read_bytes()).hexdigest(), files=exporter.hashes)
    (output / 'sources.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(f'City gate death marker: {transforms}; {len(exporter.hashes)} dependencies')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=58336)
    parser.add_argument('--output', type=Path, default=ROOT / '_private/minimum-obstacles/city-crest-source')
    args = parser.parse_args()
    run(args.port, args.output, scene='Fungus2_21', level=186, extractor=extract)
