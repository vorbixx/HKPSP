"""Recover only the original City Crest slam sprite/animation dependencies.

Run through validation-lock.ps1 -Exclusive; all source files stay immutable.
"""
import argparse
import hashlib
import json
from pathlib import Path

from assetripper_client import ROOT
from extract_crawler import Export
from extract_world_switch_session import run


def extract(client, source, output):
    rows = [r for r in client.search('Slam Effect') if r['type'] == 'GameObject'
            and r['name'] == 'Slam Effect' and r['collection'] == 'level186']
    if len(rows) != 1 or rows[0]['path']['D'] != 97:
        raise ValueError('Expected the original Fungus2_21 Slam Effect GO97')
    output.mkdir(parents=True, exist_ok=True)
    scene = rows[0]['path']['C']
    exporter = Export(client, output)
    parts = exporter.components(scene, 97)
    manifest = dict(scene='Fungus2_21', owner=97, parts=parts, dependencies={}, textures=[])
    collections = {}
    for kind, field in [('tk2dSprite', 'collection'), ('tk2dSpriteAnimator', 'library')]:
        keys = parts.get(kind, [])
        if len(keys) != 1:
            raise ValueError(f'Missing/ambiguous City slam {kind}')
        data, key, target = exporter.resolve(scene, exporter.cache[keys[0]]['m_Structure'][field])
        manifest['dependencies'][field] = dict(file=key, collection=target)
        if field == 'collection':
            collections[key] = (data, target)
        else:
            for clip in data['m_Structure']['clips']:
                print('Original slam clip:', clip['name'], clip['fps'], len(clip['frames']), clip['wrapMode'])
                for frame in clip['frames']:
                    collection, collection_key, collection_id = exporter.resolve(target, frame['spriteCollection'])
                    collections[collection_key] = (collection, collection_id)
    for key, (data, target) in collections.items():
        for ref in data['m_Structure']['textures']:
            texture, texture_key, collection_id = exporter.resolve(target, ref)
            png = texture_key.removesuffix('.json') + '.png'
            raw = client.request('/Assets/Image', Path=json.dumps({'C': collection_id, 'D': ref['m_PathID']}), Extension='png')
            (output / png).write_bytes(raw)
            exporter.hashes[png] = hashlib.sha256(raw).hexdigest()
            manifest['textures'].append(dict(file=png, collection_file=key,
                                            width=texture['m_Width'], height=texture['m_Height']))
            print('Original slam atlas:', png, texture['m_Width'], texture['m_Height'])
    manifest['sha256'] = exporter.hashes
    (output / 'sources.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=58339)
    parser.add_argument('--output', type=Path, default=ROOT / '_private/minimum-obstacles/city-crest-slam-art')
    args = parser.parse_args()
    run(args.port, args.output, scene='Fungus2_21', level=186, extractor=extract)
