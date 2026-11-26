"""Recover Crossroads_47's authored bell and grate dependencies.

Run under validation-lock.ps1 -Exclusive. Uses an isolated AssetRipper session
and immutable original inputs; output belongs to this obstacle task only.
"""
import argparse
import hashlib
import json
from functools import partial
from pathlib import Path

from assetripper_client import ROOT
from extract_crawler import Export
from extract_world_switch_session import run


def extract(client, source, output, *, scene_name='Crossroads_47', level=77, roots=(206,), include_grate=True):
    rows = [row for row in client.search('Bell')
            if row['type'] == 'GameObject' and row['collection'] == f'level{level}']
    if not rows:
        raise ValueError(f'Load the original {scene_name} bell and dependencies')
    scene = rows[0]['path']['C']
    output.mkdir(parents=True, exist_ok=True)
    exporter = Export(client, output)
    manifest = dict(scene=scene_name, parts={}, owners=[], textures=[])
    visited = set()

    def dependency(label, collection, reference, field):
        data, key, target = exporter.resolve(collection, reference)
        body = data.get('m_Structure', {})
        if not body:
            raise ValueError('Missing source MonoBehaviour data: ' + key)
        manifest['parts'][label] = dict(file=key, collection=target)
        if field == 'library':
            print(label, [(c['name'], c['fps'], len(c['frames']), c['wrapMode'])
                          for c in body['clips']], flush=True)
        else:
            for ref in body['textures']:
                texture, texture_key, texture_collection = exporter.resolve(target, ref)
                png = texture_key.removesuffix('.json') + '.png'
                if png in manifest['textures']:
                    continue
                raw = client.request('/Assets/Image', Path=json.dumps(
                    {'C': texture_collection, 'D': ref['m_PathID']}), Extension='png')
                (output / png).write_bytes(raw)
                exporter.hashes[png] = hashlib.sha256(raw).hexdigest()
                manifest['textures'].append(png)
                print('Atlas', png, texture['m_Width'], texture['m_Height'], flush=True)

    def owner(ident):
        if ident in visited:
            return
        if len(visited) >= 64:
            raise ValueError('Station hierarchy exceeds the targeted extraction bound')
        visited.add(ident)
        group = exporter.components(scene, ident)
        go = exporter.cache[group['GameObject'][0]]
        manifest['owners'].append(dict(name=go['m_Name'], id=ident, components=group))
        print('Owner', ident, go['m_Name'], list(group), flush=True)
        for kind, field in [('tk2dSprite', 'collection'), ('tk2dSpriteAnimator', 'library')]:
            for key in group.get(kind, []):
                dependency(f'{ident}-{field}', scene,
                           exporter.cache[key]['m_Structure'][field], field)
        for key in group.get('Transform', []):
            for child in exporter.cache[key]['m_Children']:
                transform, _, _ = exporter.resolve(scene, child)
                ref = transform['m_GameObject']
                if ref['m_FileID'] != 0:
                    raise ValueError('Unexpected external station child')
                owner(ref['m_PathID'])

    for ident in roots:
        owner(ident)
    grates = [row for row in client.search('Grate')
              if row['type'] == 'GameObject' and row['collection'] == f'level{level}']
    print('Scene grate candidates', [(row['name'], row['path']['D']) for row in grates], flush=True)
    if include_grate and len(grates) != 1:
        raise ValueError('Expected exactly one original Stag Grate')
    if include_grate:
        owner(grates[0]['path']['D'])
    manifest['sha256'] = exporter.hashes
    (output / 'sources.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=58332)
    parser.add_argument('--scene', choices=('Crossroads_47', 'RestingGrounds_09'), default='Crossroads_47')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    resting = args.scene == 'RestingGrounds_09'
    if args.output is None:
        args.output = ROOT / '_private/minimum-obstacles' / ('resting-station-art' if resting else 'stag-station-art')
    level = 244 if resting else 77
    run(args.port, args.output, scene=args.scene, level=level,
        extractor=partial(extract, scene_name=args.scene, level=level,
                          roots=(164, 446) if resting else (206,), include_grate=True))
