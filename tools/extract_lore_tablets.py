"""Export the three Tutorial_01 lore conversations and their HUD dependencies."""
import hashlib
import json
from pathlib import Path

from assetripper_client import Client, ROOT
from extract_crawler import Export
from extract_tutorial_prompts import decrypt_language
from inspect_fsm import decode_fsm


def extract(client=None, output=None):
    client = client or Client()
    output = output or ROOT / '_private/research/lore-tablets'
    output.mkdir(parents=True, exist_ok=True)
    export = Export(client, output)
    row = next(r for r in client.search('DialogueManager') if r['type'] == 'GameObject')
    collection = row['path']['C']
    objects, fsms, sprites = {}, {}, {}

    def visit(c, owner):
        prefix, _ = export.collection(c)
        identity = f'{prefix}:{owner}'
        if identity in objects:
            return
        parts = export.components(c, owner)
        objects[identity] = parts
        for key in parts.get('Transform', []) + parts.get('RectTransform', []):
            for ref in export.cache[key]['m_Children']:
                child, _, cc = export.resolve(c, ref)
                visit(cc, child['m_GameObject']['m_PathID'])
        for key in parts.get('PlayMakerFSM', []):
            fsms[key] = decode_fsm(export.cache[key]['m_Structure']['fsm'])
        for key in parts.get('SpriteRenderer', []):
            ref = export.cache[key]['m_Sprite']
            if not ref['m_PathID']:
                continue
            sprite, sk, sc = export.resolve(c, ref)
            texture_ref = sprite['m_RD']['m_Texture']
            _, tk, tc = export.resolve(sc, texture_ref)
            png = tk.removesuffix('.json') + '.png'
            if png not in export.hashes:
                raw = client.request('/Assets/Image', Path=json.dumps({'C': tc, 'D': texture_ref['m_PathID']}), Extension='png')
                (output / png).write_bytes(raw)
                export.hashes[png] = hashlib.sha256(raw).hexdigest()
            sprites[sk] = png

    visit(collection, row['path']['D'])
    icons = {}
    for name, owner in (('Arrow', 5742), ('Stop', 5688)):
        parts = objects[f'resources.assets:{owner}']
        component = export.cache[parts['tk2dSprite'][0]]['m_Structure']
        coll, ck, cc = export.resolve(collection, component['collection'])
        texture = coll['m_Structure']['textures'][0]
        _, tk, tc = export.resolve(cc, texture)
        png = tk.removesuffix('.json') + '.png'
        if png not in export.hashes:
            raw = client.request('/Assets/Image', Path=json.dumps({'C': tc, 'D': texture['m_PathID']}), Extension='png')
            (output / png).write_bytes(raw)
            export.hashes[png] = hashlib.sha256(raw).hexdigest()
        icons[name] = dict(collection=ck, texture=png, sprite=component['_spriteId'])
    language = next(r for r in client.search('EN_Lore Tablets') if r['type'] == 'TextAsset')
    data, _ = export.fetch(language['path']['C'], language['path']['D'])
    xml = decrypt_language(data['m_Script']).encode('utf-8')
    (output / 'EN_Lore_Tablets.xml').write_bytes(xml)
    export.hashes['EN_Lore_Tablets.xml'] = hashlib.sha256(xml).hexdigest()
    mapping = dict(build_id=22529139, objects=objects, sprites=sprites, icons=icons, source_sha256=export.hashes)
    (output / 'lore-map.json').write_text(json.dumps(mapping, indent=2) + '\n')
    (output / 'fsm-decoded.json').write_text(json.dumps(fsms, indent=2) + '\n')
    print(f'Lore tablets: {len(objects)} UI objects, {len(sprites)} sprites, {len(fsms)} FSMs')


if __name__ == '__main__':
    extract()
