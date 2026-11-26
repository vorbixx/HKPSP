"""Bake the original King's Pass TMP face into the existing HUD texture."""
import converted_files  # Install hardlink copy-on-write before asset writes.
import hashlib
import json
import math
import xml.etree.ElementTree as ET
from pathlib import Path

from PIL import Image
from assetripper_client import ROOT
from convert_hero import decode_sprite
from text_raster import source_detail


def title_data(source, title_key='KINGSPASS'):
    mapping = json.loads((source / 'title-map.json').read_text())
    for name, expected in mapping['source_sha256'].items():
        if hashlib.sha256((source / name).read_bytes()).hexdigest() != expected:
            raise ValueError('Area title source hash mismatch: ' + name)
    read = lambda name: json.loads((source / name).read_text())
    fsm = mapping['fsms']['resources.assets-24605.json']
    controller = mapping['fsms']['level6-12064.json']
    variables = lambda f, kind: {v['name']: v['value'] for v in f['variables'][kind]}
    if variables(controller, 'stringVariables')['Area Event'] != 'KINGSPASS':
        raise ValueError('Unexpected area controller')
    states = {s['name']: s for s in fsm['states']}
    wait = lambda name: next(a['parameters']['time']['value'] for a in states[name]['actions']
                             if a['type'].endswith('.Wait'))
    fade = read('resources.assets-26157.json')['m_Structure']
    text = read('resources.assets-25441.json')['m_Structure']
    font = read(mapping['font'])['m_Structure']
    entries = {e.attrib['name']: e.text or '' for e in ET.parse(source / 'EN_Titles.xml').getroot().iter('entry')}
    if entries[title_key+'_SUB'] or entries[title_key+'_SUPER']:
        raise ValueError(title_key+' unexpectedly has secondary text')
    camera = read('resources.assets-11812.json')
    # ForceCameraAspect.AutoScaleViewportShared expands height below 16:9.
    aspect = 400/240
    ppu = 120 / (camera['m_Orthographic_size'] * max(1,(16/9)/aspect))
    position = read('resources.assets-11294.json')['m_LocalPosition']
    parent = read('resources.assets-11184.json')['m_LocalPosition']
    transform = read('resources.assets-19250.json')
    # NEITHER routes through "Super only 2" before Visited Appear.
    small_y = next(a['parameters']['y']['value'] for a in states['Super only 2']['actions']
                   if a['type'].endswith('.SetPosition'))
    font_scale = text['m_fontSize']/font['m_fontInfo']['PointSize'] * .1
    # Old TMP's centered text container offsets the baseline by the font midline.
    baseline = -(font['m_fontInfo']['Ascender']+font['m_fontInfo']['Descender'])/2
    baseline *= font_scale*transform['m_LocalScale']['m_Y']
    return dict(mapping=mapping, text=entries[title_key+'_MAIN'], font=font, tmp=text,
                ppu=ppu, scale=transform['m_LocalScale']['m_X'],
                x=200 + position['m_X']*ppu, y=120-(small_y+parent['m_Y']+baseline)*ppu,
                delay=variables(controller,'floatVariables')['Visited Pause'],
                fade_in=fade['fadeInTime'], appear=wait('Visited Appear'),
                fade_out=fade['fadeOutTime'], disappear=wait('Visited Disappear'))


def raster_title(source, d):
    """Bake a localized small title with the original shared TMP face."""
    font, tmp = d['font'], d['tmp']
    glyphs = {g['id']: g for g in font['m_glyphInfoList']}
    atlas = Image.open(source / d['mapping']['atlas']).convert('RGBA').getchannel('A')
    glyph_scale = tmp['m_fontSize']/font['m_fontInfo']['PointSize'] * .1 * d['scale'] * d['ppu'] * 1.15
    pen, pieces = 0., []
    for char in d['text']:
        if char == ' ':
            advance = font['m_fontInfo']['PointSize']/4
        else:
            g = glyphs[ord(char)]
            x,y,w,h = (round(g[k]) for k in ('x','y','width','height'))
            mask = atlas.crop((x,y,x+w,y+h)).point(lambda a: 255 if a >= 128 else 0)
            pieces.append((mask, pen+g['xOffset'], -g['yOffset']))
            advance = g['xAdvance']
        pen += advance + tmp['m_characterSpacing']
    left = math.floor(min(x for _,x,y in pieces))
    top = math.floor(min(y for _,x,y in pieces))
    right = math.ceil(max(x+m.width for m,x,y in pieces))
    bottom = math.ceil(max(y+m.height for m,x,y in pieces))
    mask = Image.new('L',(right-left,bottom-top))
    for m,x,y in pieces:
        mask.paste(m,(round(x-left),round(y-top)),m)
    # Approximation: bake the source SDF face at its midpoint, then downsample;
    # PICA200 does not run TextMeshPro's distance-field shader.
    mask = mask.resize((max(1,round(mask.width*glyph_scale)),max(1,round(mask.height*glyph_scale))),Image.Resampling.LANCZOS)
    bitmap = Image.new('RGBA',mask.size,'white');bitmap.putalpha(mask)
    return bitmap, (0,(-top*glyph_scale-bitmap.height)/16,bitmap.width/16,-top*glyph_scale/16)


def raster_large_title(source, key, ppu):
    """First-visit title, using source TMP transforms and settled tk2d fleurs."""
    mapping = json.loads((source / 'large-title-map.json').read_text())
    for name, expected in mapping['source_sha256'].items():
        if hashlib.sha256((source / name).read_bytes()).hexdigest() != expected:
            raise ValueError('Large area title source hash mismatch: ' + name)
    read = lambda id: json.loads((source / f'resources.assets-{id}.json').read_text())
    font_name, atlas_name = next(iter(mapping['fonts'].items()))
    font = json.loads((source / font_name).read_text())['m_Structure']
    entries = {e.attrib['name']: e.text or '' for e in ET.parse(source / 'EN_Titles.xml').getroot().iter('entry')}
    layers = []
    canvas = Image.new('RGBA', (400,240))
    for text_id, transform_id, suffix, offset in [(25478,19249,'MAIN',0),(25100,19408,'SUPER',1.4)]:
        tmp = read(text_id)['m_Structure']
        scale = read(transform_id)['m_LocalScale']['m_X']
        data = dict(font=font,tmp=tmp,mapping=dict(atlas=atlas_name),scale=scale,ppu=ppu,text=entries[key+'_'+suffix])
        bitmap, geometry = raster_title(source,data)
        baseline = -(font['m_fontInfo']['Ascender']+font['m_fontInfo']['Descender'])/2
        baseline *= tmp['m_fontSize']/font['m_fontInfo']['PointSize']*.1*scale
        y = 120-(6.1-3.03+offset+baseline)*ppu
        canvas.alpha_composite(bitmap,(round(200-bitmap.width/2),round(y-geometry[3]*16)))
        layers.append(canvas); canvas = Image.new('RGBA',(400,240))
    collection = read(21810)['m_Structure']
    atlas = Image.open(source / mapping['collections']['resources.assets-21810.json'][0]).convert('RGBA')
    library = read(23444)['m_Structure']
    for clip_name, y in [('Top Appear',.35),('Bottom Appear',-5.24)]:
        clip = next(c for c in library['clips'] if c['name']==clip_name)
        bitmap, bounds = decode_sprite(atlas,collection['spriteDefinitions'][clip['frames'][-1]['spriteId']])
        left,bottom,right,top = bounds
        bitmap = bitmap.resize((round((right-left)*ppu),round((top-bottom)*ppu)),Image.Resampling.LANCZOS)
        canvas.alpha_composite(bitmap,(round(200+left*ppu),round(120-(6.1+y+top)*ppu)))
        layers.append(canvas); canvas = Image.new('RGBA',(400,240))
    # Approximation: settled fleurs share the title fade; source animated strokes are not replayed.
    parts = []
    for canvas in layers:
        box = canvas.getbbox()
        parts.append((canvas.crop(box),((box[0]-200)/16,(120-box[3])/16,(box[2]-200)/16,(120-box[1])/16)))
    return parts


def append_area_title(static):
    source = ROOT / '_private/research/area-title'
    d = title_data(source)
    bitmap, geometry = raster_title(source, d)
    static('Area Title 0',('area-title',0),bitmap,geometry)
    part = 1
    for key in ('WATERWAYS','ABYSS'):
        for art,bounds in raster_large_title(source,key,d['ppu']):
            static(f'Area Title {part}',('area-title',part),art,bounds)
            part += 1
    mapping = json.loads((source / 'large-title-map.json').read_text())
    for name, expected in mapping['source_sha256'].items():
        if hashlib.sha256((source / name).read_bytes()).hexdigest() != expected:
            raise ValueError('Large area title source hash mismatch: ' + name)
    read = lambda name: json.loads((source / name).read_text())
    collection = read('resources.assets-21810.json')['m_Structure']['spriteDefinitions']
    clips = read('resources.assets-23444.json')['m_Structure']['clips']
    atlas = Image.open(source / 'resources.assets-329.png').convert('RGBA')
    parent = read('resources.assets-11184.json')['m_LocalPosition']
    ornaments = []
    for index, (animation, transform) in enumerate((
            ('Top Appear', 'resources.assets-11338.json'),
            ('Bottom Appear', 'resources.assets-11443.json'))):
        # Approximation: retain the fully appeared original flourish throughout
        # the title fade instead of uploading its 22-frame reveal animation.
        clip = next(c for c in clips if c['name'] == animation)
        sprite_id = clip['frames'][-1]['spriteId']
        image, bounds = decode_sprite(atlas, collection[sprite_id])
        bounds = tuple(value * d['ppu'] / 16 for value in bounds)
        image = source_detail(image, bounds, 16, minimum_scale=0)
        static('Area Title Fleur ' + str(index), ('area-title-fleur', index), image, bounds)
        position = read(transform)['m_LocalPosition']
        ornaments.append((200 + (parent['m_X'] + position['m_X']) * d['ppu'],
                          120 - (parent['m_Y'] + position['m_Y']) * d['ppu']))
    header = '// Generated by tools/convert_area_title.py from build 22529139.\n#pragma once\nnamespace hk3ds::areatitledata {\n'
    for key,value in [('entryDelay',d['delay']),('fadeIn',d['fade_in']),('appear',d['appear']),
                      ('fadeOut',d['fade_out']),('disappear',d['disappear']),('x',d['x']),('y',d['y'])]:
        header += f'inline constexpr float {key}={value:.9f}f;\n'
    header += 'inline constexpr unsigned parts=9;\n'
    header += f'inline constexpr unsigned ornaments={len(ornaments)};\n'
    for axis, values in zip(('X', 'Y'), zip(*ornaments)):
        header += 'inline constexpr float ornament' + axis + '[ornaments]={'
        header += ','.join(f'{value:.9f}f' for value in values) + '};\n'
    header += '}\n'
    path = ROOT / 'include/area_title_data.hpp'
    if not path.exists() or path.read_text() != header:
        path.write_text(header,encoding='utf-8')
    return dict(source_sha256=d['mapping']['source_sha256'], font=d['font']['m_fontInfo']['Name'],
                size=list(bitmap.size), x=d['x'],y=d['y'],
                branch='Visited small title; KINGSPASS requires visitedCrossroads; original large-title flourishes, static reveal; no sound')
