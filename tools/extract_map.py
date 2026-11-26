"""Export only the Town/Crossroads map pieces and their authored pins."""
import hashlib
import json

from assetripper_client import Client, ROOT
from extract_crawler import Export


def extract():
    output = ROOT / '_private/research/bottom-map'
    output.mkdir(parents=True, exist_ok=True)
    client = Client()
    rows = client.search('Crossroads_') + client.search('Town') + client.search('Tutorial_01')
    collection = next(r['path']['C'] for r in rows if r['name'] == 'Crossroads_01' and
                      r['collection'] == 'resources.assets')
    sprites = {r['name']: r for r in rows if r['type'] == 'Sprite' and
               r['collection'] == 'resources.assets'}
    export = Export(client, output)
    textures = {}

    def sprite(ref):
        data, key, sc = export.resolve(collection, ref)
        _, tk, tc = export.resolve(sc, data['m_RD']['m_Texture'])
        if tk not in textures:
            name = tk.replace('.json', '.png')
            raw = client.request('/Assets/Image', Path=json.dumps({'C': tc, 'D': data['m_RD']['m_Texture']['m_PathID']}), Extension='png')
            (output / name).write_bytes(raw)
            export.hashes[name] = hashlib.sha256(raw).hexdigest()
            textures[tk] = name
        return dict(sprite=key, texture=textures[tk])

    def named_sprite(name):
        # Only source-authored sketches are exported; absent rooms stay absent.
        match = sprites.get(name)
        return sprite({'m_FileID': 0, 'm_PathID': match['path']['D']}) if match else None

    rooms = []
    # Serialized Game Map area transforms, not the separate wide-map thumbnails.
    for area, transform_id in (('Dirtmouth', 11112), ('Forgotten Crossroads', 9400)):
        root, _ = export.fetch(collection, transform_id)
        for child in root['m_Children']:
            transform, _ = export.fetch(collection, child['m_PathID'])
            parts = export.components(collection, transform['m_GameObject']['m_PathID'])
            name = export.cache[parts['GameObject'][0]]['m_Name']
            if 'SpriteRenderer' not in parts or not (name.startswith('Crossroads_') or name in ('Town', 'Tutorial_01')):
                continue
            renderer = export.cache[parts['SpriteRenderer'][0]]
            full = sprites[name]
            entry = dict(id=name, area=area, transform=parts['Transform'][0],
                         parent=root['m_LocalPosition'], color=renderer['m_Color'],
                         **sprite({'m_FileID': 0, 'm_PathID': full['path']['D']}),
                         rough=named_sprite(name + '_Cornifer'), pins=[])
            for pin_ref in transform['m_Children']:
                pin_t, _ = export.fetch(collection, pin_ref['m_PathID'])
                pin_parts = export.components(collection, pin_t['m_GameObject']['m_PathID'])
                pin_name = export.cache[pin_parts['GameObject'][0]]['m_Name']
                # Preserve the pin types used by the existing compiled map.
                if pin_name not in ('pin_bench', 'pin_stag_station', 'pin_blue_health',
                                    'pin_jiji', 'pin_mapper', 'pin_sly', 'pin_charm_slug',
                                    'pin_dream_tree', 'pin_spa', 'pin_tram'):
                    continue
                pin_renderer = export.cache[pin_parts['SpriteRenderer'][0]]
                entry['pins'].append(dict(name=pin_name, transform=pin_parts['Transform'][0],
                                          **sprite(pin_renderer['m_Sprite'])))
            rooms.append(entry)
    compass = next(r for r in client.search('Map_Knight_Pin_Compass_idle0000') if r['type'] == 'Sprite')
    marker = sprite({'m_FileID': 0, 'm_PathID': compass['path']['D']})
    mapping = dict(build_id=22529139, rooms=rooms, marker=marker, source_sha256=export.hashes)
    (output / 'map.json').write_text(json.dumps(mapping, indent=2), encoding='utf-8')
    print(f'Map: {len(rooms)} rooms, {sum(len(r["pins"]) for r in rooms)} pins, {len(textures)} source textures')


if __name__ == '__main__':
    extract()
