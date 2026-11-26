"""Recover only the original Ruins1_05b key machine and manhole art."""
import argparse
import hashlib
import json
from pathlib import Path

from assetripper_client import ROOT
from extract_crawler import Export
from extract_world_switch_session import run


def extract(client, source, output):
    rows = [r for r in client.search('Waterways Machine') if r['type'] == 'GameObject'
            and r['name'] == 'Waterways Machine' and r['collection'] == 'level93']
    if len(rows) != 1:
        raise ValueError('Load exactly one original Ruins1_05b level93')
    output.mkdir(parents=True, exist_ok=True)
    scene_collection = rows[0]['path']['C']
    exporter = Export(client, output)
    manifest = dict(scene='Ruins1_05b', parts={}, sprites={}, textures={}, scene_sources_sha256={})

    def read(ident):
        path = source / f'level93-{ident}.json'
        raw = path.read_bytes()
        manifest['scene_sources_sha256'][path.name] = hashlib.sha256(raw).hexdigest()
        return json.loads(raw)

    def part(name, collection, ref):
        data, filename, target = exporter.resolve(collection, ref)
        manifest['parts'][name] = dict(file=filename, collection=target)
        return data, target

    def sprite(collection, ref):
        data, filename, target = exporter.resolve(collection, ref)
        manifest['sprites'][filename] = dict(collection=target, path_id=ref['m_PathID'])
        texture_ref = data['m_RD']['m_Texture']
        _, key, texture_collection = exporter.resolve(target, texture_ref)
        png = key.removesuffix('.json') + '.png'
        if png not in manifest['textures']:
            raw = client.request('/Assets/Image', Path=json.dumps({'C': texture_collection, 'D': texture_ref['m_PathID']}), Extension='png')
            (output / png).write_bytes(raw)
            exporter.hashes[png] = hashlib.sha256(raw).hexdigest()
            manifest['textures'][png] = dict(collection=texture_collection, path_id=texture_ref['m_PathID'])
        return filename

    for label, renderer, animator in (('machine', 2152, 1903), ('manhole', 2091, 1893)):
        manifest['parts'][label + '_sprite'] = dict(file=sprite(scene_collection, read(renderer)['m_Sprite']))
        controller, collection = part(label + '_controller', scene_collection, read(animator)['m_Controller'])
        refs = controller['m_AnimationClips']
        if len(refs) != 1:
            raise ValueError('Unsupported Waterways controller clip count: ' + label)
        clip, clip_collection = part(label + '_clip', collection, refs[0])
        for curve in clip['m_PPtrCurves']:
            for key in curve['m_Curve']:
                if key['m_Value']['m_PathID']:
                    sprite(clip_collection, key['m_Value'])
    for label, renderer in (('machine_opened', 2151), ('manhole_opened', 2089)):
        manifest['parts'][label] = dict(file=sprite(scene_collection, read(renderer)['m_Sprite']))
    manifest['sha256'] = exporter.hashes
    (output / 'sources.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('Waterways original sprites:', len(manifest['sprites']), 'textures:', len(manifest['textures']))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=58327)
    parser.add_argument('--output', type=Path, default=ROOT / '_private/minimum-obstacles/waterways-art')
    args = parser.parse_args()
    run(args.port, args.output, scene='Ruins1_05b', level=93, extractor=extract)
