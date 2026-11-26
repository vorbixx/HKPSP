"""Export the two save-gated Tutorial_01 events from copied Unity assets."""
import argparse
import hashlib
import json
from pathlib import Path
from assetripper_client import Client, ROOT
from extract_crawler import Export
from extract_tutorial_prompts import decrypt_language
from inspect_fsm import decode_fsm


def extract(client, output):
    if not output.resolve().is_relative_to((ROOT / '_private/research').resolve()):
        raise ValueError('Original data must remain private')
    output.mkdir(parents=True, exist_ok=True)
    e = Export(client, output)
    mapping = dict(build_id=22529139, actors={}, libraries={}, textures={}, fsms={}, native_sprites={}, native_clips={}, particles={})
    def sprite(c, ref):
        data, key, sc = e.resolve(c, ref)
        texture = data['m_RD']['m_Texture']
        _, tk, tc = e.resolve(sc, texture)
        png = tk.removesuffix('.json') + '.png'
        if png not in e.hashes:
            raw = client.request('/Assets/Image', Path=json.dumps({'C': tc, 'D': texture['m_PathID']}), Extension='png')
            (output / png).write_bytes(raw)
            e.hashes[png] = hashlib.sha256(raw).hexdigest()
        mapping['native_sprites'][key] = png
        return key
    visited = set()
    def hierarchy(c, ident):
        go, key = e.fetch(c, ident)
        if key in visited:
            return
        visited.add(key)
        parts = e.components(c, ident)
        mapping['actors'][key] = parts
        for renderer in parts.get('ParticleSystemRenderer', []):
            material, mk, mc = e.resolve(c, e.cache[renderer]['m_Materials'][0])
            tex = material['m_SavedProperties']['m_TexEnvs']['_MainTex']['m_Texture']
            _, tk, tc = e.resolve(mc, tex)
            png = tk.removesuffix('.json')+'.png'
            if png not in e.hashes:
                raw = client.request('/Assets/Image', Path=json.dumps({'C':tc,'D':tex['m_PathID']}), Extension='png')
                (output/png).write_bytes(raw)
                e.hashes[png]=hashlib.sha256(raw).hexdigest()
            mapping['particles'][key]=dict(system=parts['ParticleSystem'][0], material=mk, texture=png)
        for component in go['m_Components']:
            data, ck, cc = e.resolve(c, component['m_Component'])
            if 'm_Sprite' in data and data['m_Sprite']['m_PathID']:
                mapping['native_clips'].setdefault(go['m_Name'], dict(fps=1, loop=True, frames=[sprite(cc, data['m_Sprite'])]))
            if 'm_Controller' in data and data['m_Controller']['m_PathID']:
                controller, _, ac = e.resolve(cc, data['m_Controller'])
                for ref in controller.get('m_AnimationClips', []):
                    clip, _, lc = e.resolve(ac, ref)
                    for curve in clip['m_PPtrCurves']:
                        if curve['m_Attribute'] == 'm_Sprite' and not curve['m_Path']:
                            mapping['native_clips'][go['m_Name']] = dict(fps=clip['m_SampleRate'], loop=clip['m_MuscleClip']['m_LoopTime'],
                                frames=[sprite(lc, f['m_Value']) for f in curve['m_Curve'] if f['m_Value']['m_PathID']])
        for tk in parts.get('Transform', []):
            for ref in e.cache[tk]['m_Children']:
                tr, _, tc = e.resolve(c, ref)
                hierarchy(tc, tr['m_GameObject']['m_PathID'])
    def library(label, parts, c):
        animator = e.cache[parts['tk2dSpriteAnimator'][0]]['m_Structure']
        library, key, lc = e.resolve(c, animator['library'])
        item = dict(file=key, collections={})
        for clip in library['m_Structure']['clips']:
            for frame in clip['frames']:
                ref = frame['spriteCollection']
                rk = f'{ref["m_FileID"]}:{ref["m_PathID"]}'
                if rk in item['collections']:
                    continue
                data, ck, cc = e.resolve(lc, ref)
                item['collections'][rk] = ck
                for ref in data['m_Structure']['textures']:
                    _, tk, tc = e.resolve(cc, ref)
                    png = tk.removesuffix('.json') + '.png'
                    raw = client.request('/Assets/Image', Path=json.dumps({'C': tc, 'D': ref['m_PathID']}), Extension='png')
                    (output / png).write_bytes(raw)
                    e.hashes[png] = hashlib.sha256(raw).hexdigest()
                    mapping['textures'][ck] = png
        mapping['libraries'][label] = item
    for label, query, name, collection in (
        ('mushroom', 'Mr Mushroom', 'Mr Mushroom NPC (1)', 'level6'),
        ('grimmkin', 'Flamebearer Med', 'Flamebearer Med', 'sharedassets6.assets')):
        row = next(r for r in client.search(query) if r['type'] == 'GameObject'
                   and r['name'] == name and r['collection'] == collection)
        c = row['path']['C']
        parts = e.components(c, row['path']['D'])
        hierarchy(c, row['path']['D'])
        mapping['actors'][label] = parts
        library(label, parts, c)
        for fk in parts.get('PlayMakerFSM', []):
            mapping['fsms'][fk] = decode_fsm(e.cache[fk]['m_Structure']['fsm'])
    shared = c
    for label, collection, ident in [('projectile', shared, 518), ('flash', next(r for r in client.search('Mr Mushroom') if r['name']=='Mr Mushroom NPC (1)')['path']['C'], 3690)]:
        parts = e.components(collection, ident)
        hierarchy(collection, ident)
        library(label, parts, collection)
    # Child dream dialogue and the scene spawn controller are part of the gate.
    scene = next(r for r in client.search('Mr Mushroom') if r['name'] == 'Mr Mushroom NPC (1)')['path']['C']
    for owner in (2123, 3744, 3714):
        parts = e.components(scene, owner)
        hierarchy(scene, owner)
        mapping['actors'][str(owner)] = parts
        for fk in parts.get('PlayMakerFSM', []):
            mapping['fsms'][fk] = decode_fsm(e.cache[fk]['m_Structure']['fsm'])
    # Keep decrypted original text private, just as the tutorial prompt exporter.
    for row in client.search('EN_'):
        if row['type'] == 'TextAsset' and any(x in row['name'] for x in ('Dialogue', 'Minor', 'Dream')):
            data, key = e.fetch(row['path']['C'], row['path']['D'])
            raw = decrypt_language(data['m_Script']).encode('utf-8')
            target = row['name'] + '.xml'
            (output / target).write_bytes(raw)
            e.hashes[target] = hashlib.sha256(raw).hexdigest()
    mapping['source_sha256'] = e.hashes
    (output / 'event-map.json').write_text(json.dumps(mapping, indent=2), encoding='utf-8')
    print(json.dumps(dict(files=len(e.hashes), libraries={k: v['file'] for k, v in mapping['libraries'].items()})))


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port', type=int, default=58291)
    p.add_argument('--output', type=Path, default=ROOT / '_private/research/conditional-events')
    a = p.parse_args()
    extract(Client(a.port), a.output)
