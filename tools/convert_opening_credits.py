"""Generate credits curves/layout and LCD-sized Perpetua labels in the HUD atlas."""
import converted_files  # Install hardlink copy-on-write before asset writes.
import hashlib
import json
import math
import xml.etree.ElementTree as ET
from pathlib import Path
from PIL import Image, ImageChops

ROOT = Path(__file__).resolve().parents[1]


def convert(source=ROOT / '_private/research/opening-credits', header=ROOT / 'include/opening_credits_data.hpp', lcd_scale=1.15):
    mapping = json.loads((source / 'credits-map.json').read_text())
    for filename, expected in mapping['source_sha256'].items():
        if hashlib.sha256((source / filename).read_bytes()).hexdigest() != expected:
            raise ValueError('Credits source hash mismatch: ' + filename)
    read = lambda f: json.loads((source / f).read_text())
    language = {e.attrib['name']: e.text for e in ET.fromstring((source / 'EN_Credits_List.xml').read_bytes()) if 'name' in e.attrib}
    font = json.loads((ROOT / '_private/research/tutorial-reminders/prompt-map.json').read_text())['fonts']['sharedassets0.assets-194.json']['m_fontInfo']
    root = read(mapping['objects']['2163']['Transform'])
    # Fit the native 1920x1080 overlay into 400x240 without stretching its face.
    # The resulting 400x225 image is centered vertically (7.5px above/below).
    ppu = (1080 * min(400 / 1920, 240 / 1080)) / (2 * read(mapping['camera'])['m_Orthographic_size'])
    clips = [read(f) for f in mapping['clips']]
    animation = next(c for c in clips if c['m_Name'] == 'tutorial_credits_animation')
    texts = {}
    for owner, parts in mapping['objects'].items():
        if 'TextMeshPro' not in parts:
            continue
        text = read(parts['TextMeshPro'])['m_Structure']
        tr = read(parts['Transform'])
        key = read(parts['SetTextMeshProGameText'])['m_Structure']['convName']
        value = language[key]
        go = read(parts['GameObject'])['m_Name']
        parent = next(v for v in mapping['objects'].values() if read(v['Transform'])['m_GameObject']['m_PathID'] ==
                      read(f"level6-{tr['m_Father']['m_PathID']}.json")['m_GameObject']['m_PathID'])
        path = go if parent is mapping['objects']['2163'] else read(parent['GameObject'])['m_Name'] + '/' + go
        rect = read(parts['TextContainer'])['m_Structure']['m_rect']
        if text['m_textAlignment'] != 0 or text['m_isOrthographic']:
            raise ValueError('Expected original perspective TMP top-left layout')
        # TMP non-orthographic glyph geometry is fontSize / pointSize * .1.
        font_scale = text['m_fontSize'] / font['PointSize'] * .1
        scale = root['m_LocalScale']['m_X']
        x = 200 + ppu * (root['m_LocalPosition']['m_X'] + scale * (tr['m_AnchoredPosition']['m_X'] - rect['width'] / 2))
        baseline = 120 - ppu * (root['m_LocalPosition']['m_Y'] + scale * (tr['m_AnchoredPosition']['m_Y'] + rect['height'] / 2 - font['Ascender'] * font_scale))
        glyph_scale = ppu * scale * text['m_fontSize'] * .1 / 16 * lcd_scale
        if lcd_scale != 1:
            x, baseline = round(x), round(baseline)
        texts[path] = dict(owner=int(owner), text=value, x=x, baseline=baseline, scale=glyph_scale)
    keys, rows = [], []
    for curve in animation['m_FloatCurves']:
        if curve['m_Attribute'] != 'm_fontColor.a':
            raise ValueError('Unexpected credits curve')
        row = texts[curve['m_Path']]
        row['first'] = len(keys)
        for key in curve['m_Curve']['m_Curve']:
            if key['m_WeightedMode']:
                raise ValueError('Weighted credits key requires a different evaluator')
            keys.append(tuple(key[k] for k in ('m_Time', 'm_Value', 'm_InSlope', 'm_OutSlope')))
        row['count'] = len(keys) - row['first']
        rows.append(row)
    f = lambda v: f'{v:.9f}f'
    generated = '// Generated from Build 22529139 by tools/convert_opening_credits.py.\n#pragma once\nnamespace hk3ds::creditsdata {\n'
    generated += 'struct Key {float seconds,value,inSlope,outSlope;};\nstruct Text {unsigned owner;const char* value;float x,baseline,scale;unsigned first,count;};\n'
    generated += 'inline constexpr Key keys[] = {\n' + ''.join('{' + ','.join(map(f,k)) + '},\n' for k in keys) + '};\n'
    generated += 'inline constexpr Text texts[] = {\n' + ''.join('{' + f"{r['owner']}," + json.dumps(r['text']) + ',' + ','.join(f(r[k]) for k in ('x','baseline','scale')) + f",{r['first']},{r['count']}" + '},\n' for r in rows) + '};\n}\n'
    if not header.exists() or header.read_text() != generated:
        header.write_text(generated, encoding='utf-8', newline='\n')
    return dict(source_sha256=mapping['source_sha256'], texts=rows, keys=keys,
                layout='native 1920x1080 aspect fit to 400x240; LCD-grid Perpetua face contour')


def append_labels(static, data, source=ROOT / '_private/research/tutorial-reminders'):
    """Area-sample complete lines once; never round or rescale individual glyphs."""
    mapping = json.loads((source / 'prompt-map.json').read_text())
    for filename, expected in mapping['source_sha256'].items():
        if hashlib.sha256((source / filename).read_bytes()).hexdigest() != expected:
            raise ValueError('Credit font source hash mismatch: ' + filename)
    font = mapping['fonts']['sharedassets0.assets-194.json']
    glyphs = {g['id']: g for g in font['m_glyphInfoList']}
    atlas = Image.open(source / 'sharedassets0.assets-41.png').getchannel('A')
    supersample, pad = 8, font['m_fontInfo']['Padding']
    for row in data['texts']:
        scale = row['scale'] * 16 / font['m_fontInfo']['PointSize']
        x, baseline = row['x'], row['baseline']
        pen, quads = x, []
        for char in row['text']:
            if char == ' ':
                pen += font['m_fontInfo']['PointSize'] / 4 * scale
                continue
            g = glyphs[ord(char)]
            quads.append((g, pen + (g['xOffset'] - pad) * scale,
                          baseline - (g['yOffset'] + pad) * scale))
            pen += g['xAdvance'] * scale
        left = math.floor(min(gx for _, gx, _ in quads)) - 1
        top = math.floor(min(gy for _, _, gy in quads)) - 1
        right = math.ceil(max(gx + (g['width'] + 2*pad)*scale for g, gx, _ in quads)) + 1
        bottom = math.ceil(max(gy + (g['height'] + 2*pad)*scale for g, _, gy in quads)) + 1
        mask = Image.new('L', ((right-left)*supersample, (bottom-top)*supersample))
        for g, gx, gy in quads:
            ox, oy = math.floor((gx-left)*supersample), math.floor((gy-top)*supersample)
            w = math.ceil((gx-left+(g['width']+2*pad)*scale)*supersample) - ox
            h = math.ceil((gy-top+(g['height']+2*pad)*scale)*supersample) - oy
            # Approximation: source SDF midpoint face, integrated at the final LCD
            # grid. Native-player captures independently validate contour/weight.
            tile = atlas.transform((w, h), Image.Transform.AFFINE,
                (1/(scale*supersample), 0, g['x']-pad+(ox/supersample+left-gx)/scale,
                 0, 1/(scale*supersample), g['y']-pad+(oy/supersample+top-gy)/scale),
                Image.Resampling.BILINEAR).point(lambda a: 255 if a >= 128 else 0)
            mask.paste(ImageChops.lighter(mask.crop((ox, oy, ox+w, oy+h)), tile), (ox, oy))
        mask = mask.resize((right-left, bottom-top), Image.Resampling.BOX)
        bitmap = Image.new('RGBA', mask.size, 'white')
        bitmap.putalpha(mask)
        geometry = ((left-x)/16, (baseline-bottom)/16, (right-x)/16, (baseline-top)/16)
        static(f"Credit {row['owner']}", ('credit-label', row['owner']), bitmap, geometry)


if __name__ == '__main__':
    result = convert()
    print(f"Credits: {len(result['texts'])} texts, {len(result['keys'])} Hermite keys")
