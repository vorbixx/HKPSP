"""Compile Sly's initial stock from the copied build 22529139, not prefab prices."""
import converted_files  # Own converted copy: preserve hardlink copy-on-write.
import json
import re
import struct
import textwrap
import xml.etree.ElementTree as ET
from pathlib import Path

from pickup_assets import Asset
from world_quest_assets import language_asset
from extract_tutorial_prompts import decrypt_language
import text_fragment
from tool_paths import TEX3DS as TEX3DS_EXE

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / '_private/original/Hollow Knight/hollow_knight_Data'


class Reader:
    def __init__(self, data):
        self.data, self.p = data, 32

    def take(self, fmt):
        values = struct.unpack_from('<' + fmt, self.data, self.p)
        self.p += struct.calcsize('<' + fmt)
        return values[0] if len(values) == 1 else values

    def string(self):
        n = self.take('i')
        if not 0 <= n <= len(self.data) - self.p:
            raise ValueError('Invalid Sly string')
        value = self.data[self.p:self.p+n].decode('utf-8')
        self.p = (self.p+n+3)//4*4
        return value


def compile_stock():
    scene = Asset(DATA / 'level17')
    shared = Asset(DATA / 'sharedassets17.assets')
    resources = Asset(DATA / 'resources.assets')
    expected = ('980a231c850cedd1398f32883425586a1b8e90f3666c85df057b658b1fc29418',
                'fd6dbbfb811a766c11135a026135a8f372cd5f526638178ddb2e7d117f47cb61',
                '6d49ac5a50a282a645acc32518b28e7a29d0b94a0c3b5a4cda18dade3e6fe511')
    if tuple(a.sha256 for a in (scene, shared, resources)) != expected:
        raise ValueError('Unreviewed original Sly build')
    tables = {}
    for sheet in ('Prices', 'UI'):
        _, encoded = language_asset(resources.objects, 'EN_' + sheet)
        tables[sheet] = {e.attrib['name']: e.text for e in ET.fromstring(decrypt_language(encoded))}
    stock = Reader(scene.objects[503][1])
    stock.take('iq')  # master list
    order = [stock.take('iq')[1] for _ in range(stock.take('i'))]
    if order != [39, 43, 38, 72, 67, 70, 51, 69]:
        raise ValueError('Changed initial Sly stock')
    items = {}
    for component, (kind, raw) in shared.objects.items():
        if kind != 114 or len(raw) < 32 or struct.unpack_from('<q', raw, 20)[0] != 791:
            continue
        r = Reader(raw)
        item = dict(component=component, owner=struct.unpack_from('<q', raw, 4)[0])
        for field in ('flag', 'nameKey', 'descriptionKey', 'priceKey', 'requires', 'removal'):
            item[field] = r.string()
        item['reward'] = r.take('i')
        item['name'] = tables['UI'][item['nameKey']]
        item['description'] = tables['UI'][item['descriptionKey']]
        item['price'] = int(tables['Prices'][item['priceKey']])
        items[item['owner']] = item
    result = [items[owner] for owner in order]
    if next(i['price'] for i in result if i['flag'] == 'hasLantern') != 1800:
        raise ValueError('Unexpected original Lantern price')
    raw = scene.objects[311][1]
    offset = struct.unpack_from('<2f', raw, 72)
    size = struct.unpack_from('<2f', raw, 136)
    t = scene.transform(114)
    corners = [scene.point(t, (offset[0]+sx*size[0]/2, offset[1]+sy*size[1]/2))
               for sx in (-1, 1) for sy in (-1, 1)]
    bounds = [min(p[0] for p in corners), min(p[1] for p in corners),
              max(p[0] for p in corners), max(p[1] for p in corners)]
    return result, dict(region=bounds, scene_sha256=scene.sha256, items_sha256=shared.sha256,
                        language_sha256=resources.sha256, stock_component=503)


def ascii_text(value):
    value = re.sub('<[^>]+>', ' ', value)
    return value.replace('\u2019', "'").replace('\u2018', "'").replace('\u2014', '-').replace('\ufffd', "'")



# Original Sprites exported by tools/psp/export_sly_shop_icons.py (no capture).
ICON_SOURCE = '_private/research/sly-shop-icons'


def convert_icons(items, target):
    from PIL import Image
    from convert_geo_visuals import pack_sprites
    import tex3ds_cache
    import shutil
    source = ROOT / ICON_SOURCE
    sprites = []
    hashes = {}
    import hashlib
    for item in items:
        key = item['flag']
        definition = source / (key + '.json')
        texture = source / (key + '.png')
        bitmap = Image.open(texture).convert('RGBA')
        metadata = json.loads(definition.read_text())
        if bitmap.size != (metadata['width'], metadata['height']):
            raise ValueError('Original shop icon dimensions changed: ' + key)
        box = bitmap.getchannel('A').getbbox()
        if not box:
            raise ValueError('Empty original shop icon: ' + key)
        bitmap = bitmap.crop(box)
        bitmap.thumbnail((22, 22), Image.Resampling.LANCZOS)
        sprites.append((key, bitmap, (-bitmap.width/2, -bitmap.height/2, bitmap.width/2, bitmap.height/2)))
        for path in (definition, texture):
            hashes[path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
    page = Image.new('RGBA', (128, 64))
    records = {}
    pack_sprites(sprites, page, 0, records)
    binary = target / 'sly-shop-icons.bin'
    keys = list(records)
    with binary.open('wb') as stream:
        stream.write(struct.pack('<4sIIII', b'HKA1', 1, len(keys), len(keys), len(keys)))
        for key in keys:
            stream.write(struct.pack('<5I4f', *records[key]))
        for i, key in enumerate(keys):
            stream.write(struct.pack('<48sfIIII', key.encode(), 1, 6, 0, i, 1))
        for i in range(len(keys)):
            stream.write(struct.pack('<I', i))
    png, texture = target / 'sly-shop-icons.png', target / 'sly-shop-icons.t3x'
    page.save(png)
    tex3ds_cache.run([TEX3DS_EXE, '-f', 'rgba8', '-z', 'none', '-o', str(texture), str(png)], check=True)
    for path in (binary, texture):
        shutil.copyfile(path, target.parent / 'hero/romfs' / path.name)
    (target / 'icons-source.json').write_text(json.dumps(dict(source=ICON_SOURCE, sha256=hashes, texture_bytes=128*64*4), indent=2))


def main():
    items, source = compile_stock()
    target = ROOT / '_private/converted/sly-shop'
    target.mkdir(parents=True, exist_ok=True)
    from convert_sly_actor import compile_actor
    actor_position = compile_actor(target)
    rows = []
    # Original descriptions go to the text pack; the header keeps "@Sly/<flag>".
    keys = text_fragment.write(target, 'Sly', {item['flag']: '\n'.join(textwrap.wrap(ascii_text(item['description']), 28))
                                             for item in items})
    for item in items:
        values = [item['flag'], item['requires'], item['name'], keys[item['flag']]]
        rows.append('    {' + ','.join(json.dumps(v, ensure_ascii=True) for v in values) +
                    f",{item['price']},{item['reward']}" + '},')
    output = ('// Generated by tools/convert_sly_shop.py from original serialized stock and English tables.\n'
              '#pragma once\n#include <array>\nnamespace hk3ds {\n'
              'struct SlyItem {const char* flag;const char* prerequisite;const char* name;const char* description;unsigned price,reward;};\n'
              f"inline constexpr float slyShopX={actor_position['x']}f, slyShopY={actor_position['y']}f;\n"
              'inline constexpr std::array<SlyItem,8> slyItems{{\n' + '\n'.join(rows) + '\n}};\n}\n')
    (ROOT / 'include/sly_shop_data.hpp').write_text(output, encoding='utf-8', newline='\n')
    target = ROOT / '_private/converted/sly-shop'
    target.mkdir(parents=True, exist_ok=True)
    (target.parent / 'hero/romfs/sly-shop.bin').write_bytes(struct.pack('<4s4f', b'SSP1', *source['region']))
    (target / 'source.json').write_text(json.dumps(dict(source=source, items=items), indent=2), encoding='utf-8')
    convert_icons(items, target)
    print('Compiled 8 original Sly stock records; Lantern 1800 Geo')


if __name__ == '__main__':
    main()
