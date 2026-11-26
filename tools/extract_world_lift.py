"""Recover Town lever and native cage animation art from an isolated original copy."""
import argparse
import hashlib
import json
from pathlib import Path
from assetripper_client import ROOT
from extract_crawler import Export
from extract_world_switch_session import run


def extract(client, source, output):
    rows = [r for r in client.search('Mines Lever') if r['type'] == 'GameObject'
            and r['name'] == 'Mines Lever' and r['collection'] == 'level7']
    if len(rows) != 1:
        raise ValueError('Load exactly one original Town level7')
    output.mkdir(parents=True, exist_ok=True)
    scene_collection = rows[0]['path']['C']
    exporter = Export(client, output)
    manifest = dict(scene='Town', parts={}, textures=[], cage_sprites=[], scene_sources_sha256={})
    def read(name):
        path = source / name
        if not path.exists():
            path = source.parent / 'terrain' / name
        raw = path.read_bytes()
        manifest['scene_sources_sha256'][path.relative_to(source.parent).as_posix()] = hashlib.sha256(raw).hexdigest()
        return json.loads(raw)
    def part(name, collection, ref):
        data, key, target = exporter.resolve(collection, ref)
        manifest['parts'][name] = dict(file=key, collection=target)
        return data, target
    def texture(collection, ref):
        _, key, target = exporter.resolve(collection, ref)
        png = key.removesuffix('.json') + '.png'
        if png not in manifest['textures']:
            raw = client.request('/Assets/Image', Path=json.dumps({'C': target, 'D': ref['m_PathID']}), Extension='png')
            (output / png).write_bytes(raw)
            exporter.hashes[png] = hashlib.sha256(raw).hexdigest()
            manifest['textures'].append(png)
    library, _ = part('library', scene_collection, read('level7-3815.json')['m_Structure']['library'])
    collection, cc = part('collection', scene_collection, read('level7-3791.json')['m_Structure']['collection'])
    clips = {c['name']: c for c in library['m_Structure']['clips']}
    for name in ('Lever Idle', 'Lever Hit', 'Lever Activated'):
        if name not in clips:
            raise ValueError('Missing original Town lever clip: ' + name)
    for ref in collection['m_Structure']['textures']:
        texture(cc, ref)
    animator = read('level7-2731.json')
    _, native = part('cage_controller', scene_collection, animator['m_Controller'])
    clip, clip_collection = part('cage_clip', native, dict(m_FileID=0, m_PathID=80))
    if clip['m_Name'] != 'town_mine_elev_cage0':
        raise ValueError('Changed native Town cage clip')
    for curve in clip['m_PPtrCurves']:
        for keyframe in curve['m_Curve']:
            ref = keyframe['m_Value']
            if not ref['m_PathID']:
                continue
            sprite, filename, sc = exporter.resolve(clip_collection, ref)
            if filename not in manifest['cage_sprites']:
                manifest['cage_sprites'].append(filename)
                texture(sc, sprite['m_RD']['m_Texture'])
    if len(manifest['cage_sprites']) != 11:
        raise ValueError('Changed original cage sprite sequence')
    manifest['sha256'] = exporter.hashes
    (output / 'sources.json').write_text(json.dumps(manifest, indent=2)+'\n')
    print('Town source sprites:', len(manifest['cage_sprites']), 'textures:', len(manifest['textures']))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=58327)
    parser.add_argument('--output', type=Path, default=ROOT / '_private/minimum-obstacles/town-art')
    args = parser.parse_args()
    run(args.port, args.output, scene='Town', level=7, extractor=extract)
