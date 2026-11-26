"""Recover referenced hazard markers without modifying the saved room export.

Run through validation-lock.ps1 -Exclusive. The isolated session reads the
immutable original copy; only marker dependencies and provenance are exported.
"""
import argparse
import hashlib
import json
from pathlib import Path

from assetripper_client import ROOT
from extract_crawler import Export
from extract_scenes import SCENES as EXPORTED_SCENES
from extract_world_switch_session import run

# Saved Fungus1_07 terrain collider references identify the original level136.
SCENES = dict(EXPORTED_SCENES, Fungus1_07=136)


def extract(client, visuals, output):
    terrain = visuals.parent / 'terrain'
    manifest_path = terrain / 'terrain-map.json'
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    scene = manifest['scene']
    level = SCENES[scene]
    collection_name = f'level{level}'
    # Native colliders identify the scene collection even when AssetRipper's
    # search index omits unnamed scripted marker components.
    rows = [r for r in client.search('Collider2D')
            if r['collection'] == collection_name]
    if not rows:
        raise ValueError(f'No original collider collection in {collection_name}')
    collection = rows[0]['path']['C']
    output.mkdir(parents=True, exist_ok=True)
    exporter = Export(client, output)
    references = {}
    for row in manifest['inventory']:
        for name in row['components'][0].get('HazardRespawnTrigger', []):
            saved = json.loads((terrain / name).read_text(encoding='utf-8'))
            if not saved.get('m_Enabled', True):
                continue
            original, _ = exporter.fetch(collection, int(name.rsplit('-', 1)[1][:-5]))
            if (original['m_GameObject'] != saved['m_GameObject'] or
                    original['m_Structure'] != saved['m_Structure']):
                raise ValueError(f'Saved trigger differs from original: {name}')
            pointer = original['m_Structure']['respawnMarker']
            if pointer['m_FileID'] or not pointer['m_PathID']:
                raise ValueError(f'Nonlocal/missing respawn marker: {name}')
            marker, marker_name, marker_collection = exporter.resolve(collection, pointer)
            if not isinstance(marker['m_Structure']['respawnFacingRight'], bool):
                raise ValueError(f'Invalid original marker facing: {marker_name}')
            owner = marker['m_GameObject']
            parts = exporter.components(marker_collection, owner['m_PathID'])
            transform_name = parts['Transform'][0]
            transform = exporter.cache[transform_name]
            chain = {transform_name}
            while transform['m_Father']['m_PathID']:
                transform, transform_name, marker_collection = exporter.resolve(
                    marker_collection, transform['m_Father'])
                if transform_name in chain or len(chain) >= 64:
                    raise ValueError('Cyclic/too deep hazard marker hierarchy')
                chain.add(transform_name)
            references[name] = marker_name
    if not references:
        raise ValueError(f'No enabled respawn triggers in {scene}')
    provenance = dict(build=manifest['build'], scene=scene,
                      terrain_sha256=hashlib.sha256(manifest_path.read_bytes()).hexdigest(),
                      original_level_sha256=hashlib.sha256((ROOT / '_private/original/Hollow Knight/'
                          f'hollow_knight_Data/level{level}').read_bytes()).hexdigest(),
                      references=references, source_sha256=exporter.hashes)
    (output / 'sources.json').write_text(json.dumps(provenance, indent=2) + '\n', encoding='utf-8')
    print(f'{scene}: recovered {len(references)} source respawn markers, '
          f'{len(exporter.hashes)} dependency files', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scene', choices=tuple(SCENES), default='Fungus1_01')
    parser.add_argument('--port', type=int, default=58333)
    parser.add_argument('--output', type=Path, help='Marker folder (default: respawn-markers/<scene>)')
    args = parser.parse_args()
    output = args.output or ROOT / '_private/minimum-obstacles/respawn-markers' / args.scene
    run(args.port, output, scene=args.scene, level=SCENES[args.scene], extractor=extract)
