"""Add the shiny Animator's sprite dependencies to the preserved Fury bundle."""
import hashlib
import json
from pathlib import Path

from assetripper_client import Client, ROOT
from extract_crawler import Export


def extract(client=None, output=None):
    output = output or ROOT / '_private/research/fury-chest'
    mapping = json.loads((output / 'fury-chest-map.json').read_text())
    client = client or Client()
    owner = next(r for r in client.search('Shiny Item (1)')
                 if r['collection'] == 'level6' and r['path']['D'] == 2549)
    export = Export(client, output)
    animator, animator_key, collection = export.resolve(owner['path']['C'], dict(m_FileID=0, m_PathID=8494))
    if export.hashes[animator_key] != mapping['source_sha256'][animator_key]:
        raise ValueError('Loaded shiny differs from the preserved source build')
    controller, controller_key, collection = export.resolve(collection, animator['m_Controller'])
    clip, clip_key, clip_collection = export.resolve(collection, controller['m_AnimationClips'][0])
    curves = [c for c in clip['m_PPtrCurves'] if c['m_Attribute'] == 'm_Sprite' and not c['m_Path']]
    if len(curves) != 1:
        raise ValueError('Expected one shiny sprite curve')
    frames = []
    for key in curves[0]['m_Curve']:
        sprite, sprite_key, sc = export.resolve(clip_collection, key['m_Value'])
        if sprite['m_SpriteAtlas']['m_PathID']:
            raise ValueError('Shiny requires SpriteAtlas resolution')
        _, texture_key, tc = export.resolve(sc, sprite['m_RD']['m_Texture'])
        png = texture_key.removesuffix('.json') + '.png'
        raw = client.request('/Assets/Image', Path=json.dumps(dict(C=tc, D=sprite['m_RD']['m_Texture']['m_PathID'])), Extension='png')
        if png in mapping['source_sha256'] and hashlib.sha256(raw).hexdigest() != mapping['source_sha256'][png]:
            raise ValueError('Loaded shiny texture differs from preserved build')
        (output / png).write_bytes(raw)
        export.hashes[png] = hashlib.sha256(raw).hexdigest()
        mapping['visual_assets'][sprite_key] = dict(references={'m_RD/m_Texture': texture_key})
        mapping['visual_assets'][texture_key] = dict(references={}, png=png)
        frames.append(dict(time=key['m_Time'], sprite=sprite_key))
    mapping['shiny_animation'] = dict(controller=controller_key, clip=clip_key, fps=clip['m_SampleRate'], loop=clip['m_MuscleClip']['m_LoopTime'],
                                     seconds=clip['m_MuscleClip']['m_StopTime'], frames=frames)
    for name, digest in export.hashes.items():
        if name in mapping['source_sha256'] and mapping['source_sha256'][name] != digest:
            raise ValueError('Preserved source changed: ' + name)
    mapping['source_sha256'].update(export.hashes)
    (output / 'fury-chest-map.json').write_text(json.dumps(mapping, indent=2) + '\n')
    print('Preserved shiny animation:', clip['m_Name'], len(frames), 'frames at', clip['m_SampleRate'], 'fps')


if __name__ == '__main__':
    extract()
