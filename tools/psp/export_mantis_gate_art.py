"""Export the Fungus2_04 Mantis gate tk2d library, collection and atlas.

Writes `_private/minimum-obstacles/mantis-gate-art`, read by
`tools/convert_mantis_gates.py`. The gate sprite (level168 5050) and animator
(level168 5060) come from the route export of Fungus2_04; their references are
resolved in an AssetRipper session with level168 loaded.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from assetripper_client import Client
from extract_crawler import Export
sys.path.insert(0, str(ROOT / 'tools/psp'))
from export_steam_base import collection_path

SOURCE = ROOT / '_private/research/crossroads-scenes/Fungus2_04/terrain'
PARTS = (('collection', 5050, 'collection'), ('library', 5060, 'library'))


def extract(client, source, output):
    scene = collection_path(client, 'level168')
    output.mkdir(parents=True, exist_ok=True)
    exporter = Export(client, output)
    manifest = dict(scene='Fungus2_04', parts={}, textures=[], scene_sources_sha256={})
    for label, ident, field in PARTS:
        path = source / f'level168-{ident}.json'
        raw = path.read_bytes()
        manifest['scene_sources_sha256'][path.name] = hashlib.sha256(raw).hexdigest()
        data, key, target = exporter.resolve(scene, json.loads(raw)['m_Structure'][field])
        manifest['parts'][label] = dict(file=key, collection=target)
        if label == 'collection':
            for texture_ref in data['m_Structure']['textures']:
                _, texture_key, owner = exporter.resolve(target, texture_ref)
                png = texture_key.removesuffix('.json') + '.png'
                image = client.request('/Assets/Image', Path=json.dumps({'C': owner, 'D': texture_ref['m_PathID']}),
                                       Extension='png')
                (output / png).write_bytes(image)
                exporter.hashes[png] = hashlib.sha256(image).hexdigest()
                manifest['textures'].append(png)
    manifest['sha256'] = exporter.hashes
    (output / 'sources.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Mantis gate art: {len(manifest["textures"])} atlas, {len(exporter.hashes)} files')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--port', type=int, default=58291)
    parser.add_argument('--source', type=Path, default=SOURCE)
    parser.add_argument('--output', type=Path, default=ROOT / '_private/minimum-obstacles/mantis-gate-art')
    args = parser.parse_args()
    extract(Client(args.port), args.source, args.output)


if __name__ == '__main__':
    main()
