"""Preserve and pack the original inventory icons into one small private atlas."""
import converted_files  # Install hardlink copy-on-write before asset writes.
import argparse
import hashlib
import json
import shutil
import struct
import tex3ds_cache
from pathlib import Path
from PIL import Image
from assetripper_client import Client, ROOT
from extract_crawler import Export
from convert_room_visuals import decode
from convert_geo_visuals import pack_sprites
from tool_paths import TEX3DS as TEX3DS_EXE

# Stable names in source build 22529139. Ownership is decided at runtime.
ICONS = {
    'Nail': 'Inv_0033_inv_nail_01',
    'Focus': 'Inv_0029_spell_core',
    'Cloak': 'Inv_0015_dash-cloak',
    'Claw': 'items__0003_mantis-claw',
    'Wings': 'items__0002_emperor-wings',
    'ShadeCloak': 'items__0004_shade_cloak',
    'CityCrest': 'inv_item_city_key',
    'Spirit': 'Inv_0025_spell_fireball_01',
    'Dive': 'Inv_0026_spell_quake_01',
    'Shriek': 'Inv_0024_spell_scream_01',
    'Spirit2': 'Inv_0025_spell_fireball_01_level2',
    'Dive2': 'Inv_0026_spell_quake_01_level2',
    'Shriek2': 'Inv_0024_spell_scream_01_level2',
    'Vessel': 'Inventory_soul_vessel_full',
    'Mask': 'Inv_0030_inv_health_backboard',
    'Geo': 'InventoryCoin',
    'Cursor': 'Inv_0014_selection_cursor',
    'Divider': 'Inv_0017_divider',
}

# CharmIconList (resources.assets:21351), spriteList[PlayerData charm ID].
CHARM_ICONS = {'Charm1': 'charm_sprite_02',
 'Charm2': 'charm_sprite_03',
 'Charm3': 'charm_grub_mid',
 'Charm4': '_0006_charm_stalwart_shell',
 'Charm5': 'charm_blocker',
 'Charm6': '_0005_charm_fury',
 'Charm7': '_0005_charm_fast_focus',
 'Charm8': '_0010_charm_bluehealth',
 'Charm12': '_0000_charm_thorn_counter',
 'Charm14': '_0006_charm_no_recoil',
 'Charm18': '_0007_charm_greed',
 'Charm19': '_0002_charm_spell_damage_up',
 'Charm20': '_0001_charm_more_soul',
 'Charm22': '_0009_charm_Hatchling'}
ICONS.update(CHARM_ICONS)


def extract(source, client=None):
    source.mkdir(parents=True, exist_ok=True)
    client = client or Client()
    export = Export(client, source)
    found = {}
    for query in ('Inv_', 'items__', 'Inventory', 'charm'):
        for item in client.search(query):
            if item['type'] == 'Sprite' and item['collection'] == 'resources.assets':
                found[item['name']] = item['path']
    collection = found[ICONS['Nail']]['C']
    # Compare a native sprite: a running analysis server may omit managed clip
    # structures when its assemblies were not loaded, without changing assets.
    sprite, _ = export.fetch(collection, 3032)
    known = ROOT/'_private/research/hud-feedback/resources.assets-3032.json'
    if sprite != json.loads(known.read_text()):
        raise ValueError('Inventory resources differ from the preserved source build')
    binding, _ = export.fetch(collection, 21351)
    references = binding['m_Structure']['spriteList']
    for name, original in CHARM_ICONS.items():
        sprite, _, _ = export.resolve(collection, references[int(name[5:])])
        if sprite['m_Name'] != original:
            raise ValueError(f'Original charm binding differs for {name}')
    icons = {}
    for name, original in ICONS.items():
        path = found[original]
        sprite, filename = export.fetch(path['C'], path['D'])
        _, texture_key, collection = export.resolve(path['C'], sprite['m_RD']['m_Texture'])
        png = texture_key.replace('.json', '.png')
        if not (source/png).exists():
            raw = client.request('/Assets/Image', Path=json.dumps(
                {'C': collection, 'D': sprite['m_RD']['m_Texture']['m_PathID']}), Extension='png')
            (source/png).write_bytes(raw)
        export.hashes[png] = hashlib.sha256((source/png).read_bytes()).hexdigest()
        icons[name] = dict(sprite=filename, texture=png, original=original)
    (source/'inventory-map.json').write_text(json.dumps(dict(
        build_id=22529139, icons=icons, source_sha256=export.hashes), indent=2))


def convert(source, output, romfs, tex3ds):
    mapping = json.loads((source/'inventory-map.json').read_text())
    sprites = {}
    for name, entry in mapping['icons'].items():
        sprite = json.loads((source/entry['sprite']).read_text())
        bitmap, _ = decode(Image.open(source/entry['texture']).convert('RGBA'), sprite)
        # Compact LCD layout; preserve the original bitmap aspect ratio.
        if bitmap.getbbox(): bitmap = bitmap.crop(bitmap.getbbox())
        limit = 140 if name == 'Divider' else 32 if name.startswith('Charm') else 128 if name=='Nail' else 32 if name=='Cursor' else 36
        bitmap.thumbnail((limit, 12 if name == 'Divider' else limit), Image.Resampling.LANCZOS)
        sprites[name] = bitmap, (-bitmap.width/2, -bitmap.height/2, bitmap.width/2, bitmap.height/2)
    logo_source = source.parent/'title-menu/title-source.png'
    source_hashes = dict(mapping['source_sha256'])
    source_hashes['../title-menu/title-source.png'] = hashlib.sha256(logo_source.read_bytes()).hexdigest()
    logo = Image.open(logo_source).convert('RGBA')
    logo = logo.crop(logo.getchannel('A').getbbox())
    logo.thumbnail((224, 72), Image.Resampling.LANCZOS)
    sprites['Logo'] = logo, (-logo.width/2,-logo.height/2,logo.width/2,logo.height/2)
    page = Image.new('RGBA', (256, 256))
    records = {}
    # Keep the long flourish in a strip so it does not fragment the icon rows.
    divider, bounds = sprites.pop('Divider')
    icons = Image.new('RGBA', (256, 244))
    pack_sprites([(name, *value) for name, value in sprites.items()], icons, 0, records,
                 shared_gutters=True)
    page.paste(icons, (0, 0))
    page.paste(divider, (1, 245))
    records['Divider'] = (0, 1, 245, divider.width, divider.height, *bounds)
    output.mkdir(parents=True, exist_ok=True)
    romfs.mkdir(parents=True, exist_ok=True)
    binary = output/'inventory.bin'
    keys = list(records)
    with binary.open('wb') as stream:
        stream.write(struct.pack('<4sIIII', b'HKA1', 1, len(keys), len(keys), len(keys)))
        for key in keys:
            stream.write(struct.pack('<5I4f', *records[key]))
        for i, key in enumerate(keys):
            stream.write(struct.pack('<48sfIIII', key.encode(), 1, 6, 0, i, 1))
        for i in range(len(keys)):
            stream.write(struct.pack('<I', i))
    png, texture = output/'inventory-page-0.png', output/'inventory-page-0.t3x'
    page.save(png)
    tex3ds_cache.run([str(tex3ds), '-f', 'rgba4', '-z', 'none', '-o', str(texture), str(png)], check=True)
    for path in (binary, texture):
        shutil.copyfile(path, romfs/path.name)
    (output/'manifest.json').write_text(json.dumps(dict(
        build_id=mapping['build_id'], icons=mapping['icons'],
        source_sha256=source_hashes, texture_bytes=256*256*2,
        texture_format='rgba4'), indent=2))
    print(f'Inventory: {len(keys)} original icons, one 128 KiB RGBA4 atlas')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--extract', action='store_true')
    parser.add_argument('--source', type=Path, default=ROOT/'_private/research/inventory')
    parser.add_argument('--output', type=Path, default=ROOT/'_private/converted/inventory')
    parser.add_argument('--romfs', type=Path, default=ROOT/'_private/converted/hero/romfs')
    parser.add_argument('--tex3ds', type=Path, default=Path(TEX3DS_EXE))
    args = parser.parse_args()
    if args.extract:
        extract(args.source)
    convert(args.source, args.output, args.romfs, args.tex3ds)
