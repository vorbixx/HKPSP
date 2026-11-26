"""Export Crossroads_03's original switch/gate animation dependencies.

Requires an isolated AssetRipper session with level39, its dependencies and
Managed loaded. Run under validation-lock.ps1 -Exclusive; original inputs stay
immutable. This export does not admit the room or change the save schema.
"""
import argparse
import hashlib
import json
from pathlib import Path
from assetripper_client import Client, ROOT
from extract_crawler import Export


def extract(client, source, output):
    output = output.absolute()
    if not output.is_relative_to(ROOT / '_private'):
        raise ValueError('Original assets must remain private')
    rows = [r for r in client.search('Toll Gate Switch')
            if r['type'] == 'GameObject' and r['name'] == 'Toll Gate Switch'
            and r['collection'] == 'level39']
    if len(rows) != 1:
        raise ValueError('Load exactly one level39 with its original dependencies')
    output.mkdir(parents=True, exist_ok=True)
    scene_collection = rows[0]['path']['C']
    exporter = Export(client, output)
    manifest = dict(scene='Crossroads_03', scene_collection=scene_collection,
                    parts={}, scene_sources_sha256={}, textures=[])
    for kind, filename, field in [('library', 'level39-4690.json', 'library'),
                                  ('collection', 'level39-4594.json', 'collection')]:
        raw = (source / filename).read_bytes()
        manifest['scene_sources_sha256'][filename] = hashlib.sha256(raw).hexdigest()
        structure = json.loads(raw)['m_Structure']
        data, key, target = exporter.resolve(scene_collection, structure[field])
        body = data.get('m_Structure', {})
        if not body:
            raise ValueError('Missing MonoBehaviour data; load globalgamemanagers and Managed')
        manifest['parts'][kind] = dict(file=key, collection=target)
        if kind == 'library':
            clips = {c['name']: c for c in body['clips']}
            for name in ('Switch Idle', 'Switch Hit', 'Toll Gate Open'):
                clip = clips[name]
                print(name, clip['fps'], len(clip['frames']), clip['wrapMode'])
        else:
            for ref in body['textures']:
                texture, texture_key, texture_collection = exporter.resolve(target, ref)
                png = texture_key.removesuffix('.json') + '.png'
                raw = client.request('/Assets/Image', Path=json.dumps(
                    {'C': texture_collection, 'D': ref['m_PathID']}), Extension='png')
                (output / png).write_bytes(raw)
                exporter.hashes[png] = hashlib.sha256(raw).hexdigest()
                manifest['textures'].append(png)
                print('texture', png, texture['m_Width'], texture['m_Height'])
    manifest['sha256'] = exporter.hashes
    (output / 'sources.json').write_text(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=58305)
    parser.add_argument('--source', type=Path, default=ROOT / '_private/research/crossroads-scenes/Crossroads_03/visuals')
    parser.add_argument('--output', type=Path, default=ROOT / '_private/minimum-obstacles/switch-art')
    args = parser.parse_args()
    extract(Client(args.port), args.source, args.output)
