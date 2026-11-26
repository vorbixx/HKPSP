"""Bake the source visited Mantis Lords title into the resident encounter bank."""
import json
import math
import xml.etree.ElementTree as ET

from PIL import Image, ImageChops

from assetripper_client import ROOT
from convert_area_title import raster_title, title_data


def readable_super(bitmap):
    """Keep the source face, with readable size and stroke weight at 400x240."""
    bounds = bitmap.getchannel('A').point(lambda a: 255 if a >= 128 else 0).getbbox()
    ink = bitmap.crop(bounds)
    if ink.height < 10:
        ink = ink.resize((round(ink.width * 10 / ink.height), 10), Image.Resampling.LANCZOS)
    # A second horizontal sample preserves the Trajan face while giving its
    # subpixel stems a full native pixel. Empty padding prevents wraparound.
    weighted = Image.new('RGBA', (ink.width + 2, ink.height))
    weighted.alpha_composite(ink, (0, 0))
    ink = ImageChops.lighter(weighted, ImageChops.offset(weighted, 1, 0))
    # A small quiet backing prevents the thin source face blending into scenery.
    backed = Image.new('RGBA', (ink.width + 4, ink.height + 4), (0, 0, 0, 208))
    backed.alpha_composite(ink, (2, 2))
    return backed


def title_sprite():
    source = ROOT / '_private/research/area-title'
    read = lambda name: json.loads((source / name).read_text())
    data = title_data(source)
    entries = {e.attrib['name']: e.text or ''
               for e in ET.parse(source / 'EN_Titles.xml').getroot().iter('entry')}
    if entries['MANTIS_LORDS_SUB']:
        raise ValueError('Unexpected Mantis Lords subtitle')
    fsm = data['mapping']['fsms']['resources.assets-24605.json']
    states = {s['name']: s for s in fsm['states']}
    position = next(a['parameters'] for a in states['Set Text Small']['actions']
                    if a['type'].endswith('.SetPosition'))
    folder_y = next(a['parameters']['y']['value'] for a in states['Super only 2']['actions']
                    if a['type'].endswith('.SetPosition'))
    parent = read('resources.assets-11184.json')['m_LocalPosition']
    # Native 16:9 source, center-cropped to the 3DS viewport; retain its vertical scale.
    ppu = 120 / read('resources.assets-11812.json')['m_Orthographic_size']
    parts = []
    for key, transform_id, text_id in [('MAIN', 19250, 25441), ('SUPER', 19299, 24618)]:
        transform = read(f'resources.assets-{transform_id}.json')
        tmp = read(f'resources.assets-{text_id}.json')['m_Structure']
        scale = transform['m_LocalScale']
        anchor = transform['m_AnchoredPosition']
        line = dict(data, text=entries['MANTIS_LORDS_' + key], tmp=tmp,
                    scale=scale['m_X'], ppu=ppu)
        bitmap, geometry = raster_title(source, line)
        font = data['font']['m_fontInfo']
        midline = -(font['Ascender'] + font['Descender']) / 2
        midline *= tmp['m_fontSize'] / font['PointSize'] * .1 * scale['m_Y']
        x = 200 + (position['x']['value'] + parent['m_X'] + anchor['m_X']) * ppu
        baseline = 120 - (folder_y + parent['m_Y'] + anchor['m_Y'] + midline) * ppu
        left, top = x + geometry[0] * 16, baseline - geometry[3] * 16
        if key == 'SUPER':
            readable = readable_super(bitmap)
            # Retain the source line's center and bottom; grow away from LORDS.
            left += (bitmap.width - readable.width) / 2
            top += bitmap.height - readable.height + 2
            bitmap = readable
        parts.append((bitmap, left, top))
    # Enlarging the small line can push it beyond the source's left margin.
    # Move both lines together so their relative alignment survives at 400x240.
    inset = max(0, 2 - min(x for _, x, _ in parts))
    parts = [(bitmap, x + inset, y) for bitmap, x, y in parts]
    left = math.floor(min(x for _, x, _ in parts))
    top = math.floor(min(y for _, _, y in parts))
    right = math.ceil(max(x + im.width for im, x, _ in parts))
    bottom = math.ceil(max(y + im.height for im, _, y in parts))
    assert 0 <= left < right <= 400, 'Native boss title must fit the screen'
    title = Image.new('RGBA', (right - left, bottom - top))
    for bitmap, x, y in parts:
        title.alpha_composite(bitmap, (round(x - left), round(y - top)))
    return ('Mantis Title', title, (left / 16, -bottom / 16, right / 16, -top / 16))
