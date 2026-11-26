"""Pack verified original lore text and trigger bounds; no source text in code."""
import converted_files  # Install hardlink copy-on-write before asset writes.
import hashlib
import json
import struct
import xml.etree.ElementTree as ET
from pathlib import Path
from PIL import Image
from assetripper_client import ROOT
from text_raster import sdf_glyph
from top_screen_ui_layout import LAYOUT

TEXT_SCALE = .8
TEXT_WIDTH = 296
TEXT_LEFT = 12
TEXT_BASELINE = 78
LINE_HEIGHT = 13
PARAGRAPH_HEIGHT = 5
FLEUR_TOP = 62
FLEUR_BOTTOM = 144
FOCUS_FLEUR_BOTTOM = 118
FLEUR_SCALE = .8
MARKER_SCALE = 1.3


def wrap_page(value, advances):
    lines = []
    for paragraph in value.replace('<br>', '\n').strip().split('\n'):
        line = ''
        for word in paragraph.split():
            candidate = (line + ' ' + word).strip()
            if line and sum(advances[ord(c)] for c in candidate) * TEXT_SCALE > TEXT_WIDTH:
                lines.append(line)
                line = word
            else:
                line = candidate
        lines.append(line)
    return '\n'.join(lines)


def append_lore(static, romfs):
    source = ROOT / '_private/research/lore-tablets'
    mapping = json.loads((source / 'lore-map.json').read_text())
    for name, expected in mapping['source_sha256'].items():
        if hashlib.sha256((source / name).read_bytes()).hexdigest() != expected:
            raise ValueError('Lore source hash mismatch: ' + name)
    prompts = json.loads((ROOT / '_private/research/tutorial-reminders/prompt-map.json').read_text())
    font = prompts['fonts']['sharedassets0.assets-194.json']
    advances = {g['id']: g['xAdvance'] * 16 / font['m_fontInfo']['PointSize'] for g in font['m_glyphInfoList']}
    advances[32] = 4
    entries = {e.attrib['name']: e.text for e in ET.parse(source / 'EN_Lore_Tablets.xml').getroot().iter('entry')}
    tablets = ROOT / '_private/research/tablet-glow'
    table = json.loads((tablets / 'tablet-glow-map.json').read_text())
    payload = bytearray(b'HKL1')
    characters = set('Inspect^v')
    audit = []
    for t, key in zip(table['tablets'], ('TUT_TAB_02', 'TUT_TAB_01', 'TUT_TAB_03')):
        read = lambda name: json.loads((tablets / name).read_text())
        transform = read(t['parts']['Transform'][0])
        collider = read(t['parts']['BoxCollider2D'][0])
        vector = lambda v: {k: v['m_' + k.upper()] for k in ('x', 'y')}
        p, s = vector(transform['m_LocalPosition']), vector(transform['m_LocalScale'])
        o, size = vector(collider['m_Offset']), vector(collider['m_Size'])
        bounds = [p['x'] + (o['x'] - size['x']/2)*s['x'], p['y'] + (o['y'] - size['y']/2)*s['y'],
                  p['x'] + (o['x'] + size['x']/2)*s['x'], p['y'] + (o['y'] + size['y']/2)*s['y']]
        pages = [wrap_page(p, advances) for p in entries[key].split('<page>')]
        if max(len(p.splitlines()) for p in pages) > 5:
            raise ValueError('Lore page exceeds lower-screen text area')
        payload += struct.pack('<I4fI', t['owner'], *bounds, len(pages))
        for page in pages:
            encoded = page.encode('ascii')
            if not 0 < len(encoded) < 768:
                raise ValueError('Lore page exceeds fixed runtime capacity')
            payload += struct.pack('<I', len(encoded)) + encoded
            characters.update(page.replace('\n', ''))
        audit.append(dict(owner=t['owner'], key=key, bounds=bounds, pages=len(pages), lines=[len(p.splitlines()) for p in pages]))
    (romfs / 'lore-tablets.bin').write_bytes(payload)
    # Approximation: source black vignette adapted to the lower LCD's text area.
    image = Image.open(source / mapping['sprites']['resources.assets-3233.json']).convert('RGBA')
    sprite = json.loads((source / 'resources.assets-3233.json').read_text())
    from convert_room_visuals import decode
    bitmap, _ = decode(image, sprite)
    bitmap = bitmap.resize((32, 14), Image.Resampling.LANCZOS)
    static('Lore Backboard', ('lore-backboard', 0), bitmap, (-9.375, -3.125, 9.375, 3.125))
    from convert_hero import decode_sprite
    for name, icon in mapping['icons'].items():
        collection = json.loads((source / icon['collection']).read_text())['m_Structure']
        bitmap, geometry = decode_sprite(Image.open(source / icon['texture']), collection['spriteDefinitions'][icon['sprite']])
        bitmap = bitmap.resize((max(1, round(bitmap.width*.5)), max(1, round(bitmap.height*.5))), Image.Resampling.LANCZOS)
        static('Lore ' + name, ('lore-icon', name), bitmap, geometry)
    # The settled frames of the source dialogue flourishes share the icon atlas.
    collection = json.loads((source / mapping['icons']['Stop']['collection']).read_text())['m_Structure']
    atlas = Image.open(source / mapping['icons']['Stop']['texture'])
    for name, index in [('Top', 35), ('Bottom', 44)]:
        bitmap, geometry = decode_sprite(atlas, collection['spriteDefinitions'][index])
        # Integrate hairline curls at their final native menu sizes, rather
        # than dropping strokes when the LCD samples an oversized bitmap.
        for percent in ((35, 55, 70) if name == 'Top' else (35, 45)):
            left, bottom, right, top = geometry
            size = (max(1, round((right-left)*16*percent/100)),
                    max(1, round((top-bottom)*16*percent/100)))
            native = bitmap.convert('RGBa').resize(size, Image.Resampling.BOX).convert('RGBA')
            static(f'Menu Fleur {name} {percent}', ('menu-fleur', name, percent), native, geometry)
        bitmap = bitmap.resize((round(bitmap.width*.25), round(bitmap.height*.25)), Image.Resampling.LANCZOS)
        static('Lore Fleur ' + name, ('lore-fleur', name), bitmap, geometry)
    # Bake at the final LCD size to avoid filtering an already reduced 16px glyph.
    atlas = Image.open(ROOT / '_private/research/tutorial-reminders/sharedassets0.assets-41.png').getchannel('A')
    for glyph in font['m_glyphInfoList']:
        code = glyph['id']
        if chr(code) not in characters or code == 32:
            continue
        bitmap, geometry = sdf_glyph(atlas, glyph, font, 16*TEXT_SCALE,
                                     raster_scale=LAYOUT['panelScale'])
        static(f'Lore Glyph {code}', ('lore-glyph',code), bitmap, geometry)
    values = dict(textScale=TEXT_SCALE,textLeft=TEXT_LEFT,textBaseline=TEXT_BASELINE,
                  lineHeight=LINE_HEIGHT,paragraphHeight=PARAGRAPH_HEIGHT,
                  fleurTop=FLEUR_TOP,fleurBottom=FLEUR_BOTTOM,focusFleurBottom=FOCUS_FLEUR_BOTTOM,fleurScale=FLEUR_SCALE,markerScale=MARKER_SCALE)
    generated = '// Generated by convert_lore_tablets.py; lower LCD adaptation of the native dialogue.\n#pragma once\nnamespace hk3ds::lorelayout {\n'
    generated += ''.join(f'inline constexpr float {key} = {value:.9f}f;\n' for key,value in values.items()) + '}\n'
    header = ROOT / 'include/lore_tablet_layout.hpp'
    if not header.exists() or header.read_text()!=generated:
        header.write_text(generated, newline='\n')
    (romfs / 'lore-tablets-manifest.json').write_text(json.dumps(dict(build_id=22529139, tablets=audit,
        source_sha256=mapping['source_sha256'], text_scale=TEXT_SCALE, text_width=TEXT_WIDTH), indent=2) + '\n')
    return characters
