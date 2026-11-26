"""Resolve only Tutorial_01 plant Animator dependencies into a private directory.

Every root component is compared with the saved room export before resolving
controller/clip/sprite references through the live collection dependency table.
This exports observations, not a native Animator or a claim of visual parity.
"""
import argparse
import hashlib
import json
from pathlib import Path

from assetripper_client import Client, ROOT
from extract_crawler import Export

BEHAVIOR = '56bdc7acac038683c77fe7f1f19f00f534fa2039296e8bda42ce1864e7c26af0'


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def extract(client, source, output):
    roots = [(ROOT / '_private' / name).resolve() for name in ('research', 'converted')]
    if not any(output.resolve().is_relative_to(root) for root in roots):
        raise ValueError('Plant exports must remain private')
    manifest_raw = (source / 'room-visuals-map.json').read_bytes()
    manifest = json.loads(manifest_raw)
    if (manifest['build'], manifest['scene']) != (22529139, 'Tutorial_01'):
        raise ValueError('Unexpected source build/scene')
    behavior = ROOT / '_private/research/decompiled/all/PlayFromRandomFrameMecanim.cs'
    if digest(behavior.read_bytes()) != BEHAVIOR:
        raise ValueError('Random-start behavior changed; recheck observations')
    rows = [r for r in manifest['inventory']
            if r['active'] and 'Breakable' in r['components']
            and 'PlayFromRandomFrameMecanim' in r['components']]
    rows = list({r['go']: r for r in rows}.values())
    live = {r['path']['D']: r for r in client.search('health plant')
            if r['type'] == 'GameObject' and r['collection'] == 'level6'}
    output.mkdir(parents=True, exist_ok=True)
    export = Export(client, output)
    anchors, plants, textures = {}, [], {}
    for row in rows:
        owner = int(row['go'][7:-5])
        if owner not in live:
            raise ValueError('Plant root absent from live session')
        collection = live[owner]['path']['C']
        go, key = export.fetch(collection, owner)
        keys = [key]
        animator = None
        for entry in go['m_Components']:
            data, key, _ = export.resolve(collection, entry['m_Component'])
            keys.append(key)
            if 'm_Controller' in data:
                if animator is not None:
                    raise ValueError('Multiple plant Animators')
                animator = (data, key)
        for key in keys:
            saved = (source / key).read_bytes()
            if digest(saved) != manifest['source_sha256'][key] or export.hashes[key] != digest(saved):
                raise ValueError('Live plant anchor differs: ' + key)
            anchors[key] = digest(saved)
        if animator is None:
            raise ValueError('Missing plant Animator')
        controller, controller_key, cc = export.resolve(collection, animator[0]['m_Controller'])
        clips = []
        for ref in controller['m_AnimationClips']:
            clip, clip_key, clip_collection = export.resolve(cc, ref)
            clips.append(clip_key)
            for curve in clip['m_PPtrCurves']:
                for frame in curve['m_Curve']:
                    sprite, sprite_key, sc = export.resolve(clip_collection, frame['m_Value'])
                    texture, texture_key, tc = export.resolve(sc, sprite['m_RD']['m_Texture'])
                    if texture_key not in textures:
                        image_name = texture_key.removesuffix('.json') + '.png'
                        image = client.request('/Assets/Image',
                            Path=json.dumps({'C': tc, 'D': sprite['m_RD']['m_Texture']['m_PathID']}),
                            Extension='png')
                        (output / image_name).write_bytes(image)
                        export.hashes[image_name] = digest(image)
                        textures[texture_key] = image_name
        plants.append(dict(owner=owner, animator=animator[1], controller=controller_key,
                           clips=clips, random_start=row['components']['PlayFromRandomFrameMecanim'][0],
                           renderer=row['renderer']))
    report = dict(build=manifest['build'], scene=manifest['scene'], plants=plants,
                  source_sha256=export.hashes, room_anchors=anchors, textures=textures,
                  room_manifest_sha256=digest(manifest_raw), behavior_sha256=BEHAVIOR,
                  limitation='Live root components match saved export; full session input provenance is not established.')
    (output / 'manifest.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=58291)
    args = parser.parse_args()
    report = extract(Client(args.port), ROOT / '_private/research/room-visuals',
                     ROOT / '_private/research/plant-dependencies')
    print(json.dumps(dict(plants=len(report['plants']), hashed_files=len(report['source_sha256']))))
