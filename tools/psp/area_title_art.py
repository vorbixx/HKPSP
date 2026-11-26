"""Native overlays from the original TMP face and tk2d area-title atlas."""
import json
import struct
import sys
import xml.etree.ElementTree as ET

import transcode_room as t


def layers(root, ppu):
    image = t.import_pil()
    sys.path.insert(0, str(t.REPO / 'tools'))
    from convert_area_title import raster_title
    from convert_hero import decode_sprite

    source = root / '_private/research/area-title'
    def read(asset):
        return json.loads((source / f'resources.assets-{asset}.json').read_text(encoding='utf-8'))

    mapping = json.loads((source / 'large-title-map.json').read_text(encoding='utf-8'))
    font_name, atlas_name = next(iter(mapping['fonts'].items()))
    font = json.loads((source / font_name).read_text(encoding='utf-8'))['m_Structure']
    entries = {entry.attrib['name']: entry.text for entry in
               ET.parse(source / 'EN_Titles.xml').getroot().iter('entry')}
    collection = read(21810)['m_Structure']['spriteDefinitions']
    library = read(23444)['m_Structure']['clips']
    atlas = image.open(source / mapping['collections']['resources.assets-21810.json'][0]).convert('RGBA')
    title_ppu = ppu / .85

    def text(key, tmp, transform):
        bitmap, _ = raster_title(source, dict(font=font, tmp=read(tmp)['m_Structure'],
                                 mapping=dict(atlas=atlas_name), scale=read(transform)['m_LocalScale']['m_X'],
                                 ppu=title_ppu / 1.15, text=entries[key]))
        return bitmap.crop(bitmap.getbbox())

    def fleur(name, offset):
        clip = next(clip for clip in library if clip['name'] == name)
        frames = []
        for frame in clip['frames']:
            bitmap, (left, bottom, right, top) = decode_sprite(atlas, collection[frame['spriteId']])
            bitmap = bitmap.resize((max(1, round((right-left)*title_ppu)), max(1, round((top-bottom)*title_ppu))),
                                   image.Resampling.LANCZOS)
            frames.append((bitmap, round(240+left*title_ppu), round(136-(6.1+offset+top)*title_ppu)))
        return clip['fps'], frames

    for name, offset in (('Top', .35), ('Bottom', -5.24)):
        yield 'Region Fleur '+name, *fleur(name+' Appear', offset)
        yield 'Region Fleur '+name+' Down', *fleur(name+' Disappear', offset)

    for name, large_y, small_y, secondary, large_secondary_y, small_secondary_y in (
            ('DIRTMOUTH', 61, 223, 'SUB', 87, 243),
            ('CROSSROADS', 74, 235, 'SUPER', 61, 224),
            ('GREENPATH', 61, 235, None, 0, 0)):
        large = text(name+'_MAIN', 25478, 19249)
        small = text(name+'_MAIN', 25441, 19250)
        yield f'{name} Large', 1, [(large, 240-large.width//2, large_y+1)]
        yield f'{name} Small', 1, [(small, 29, small_y+1)]
        if secondary:
            large_part, small_part = ((25254, 19327), (25913, 19182)) if secondary == 'SUB' else (
                (25100, 19408), (24618, 19299))
            large_text = text(name+'_'+secondary, *large_part)
            small_text = text(name+'_'+secondary, *small_part)
            yield f'{name} Large Extra', 1, [(large_text, 240-large_text.width//2, large_secondary_y+1)]
            yield f'{name} Small Extra', 1, [(small_text, 29+(small.width-small_text.width)//2, small_secondary_y+1)]


def append(root, ppu, table, baked):
    result = dict(baked)
    clips = bytearray(table.clip_bytes)
    frames = list(table.frames)
    index = len(table.sprites)
    for name, fps, artwork in layers(root, ppu):
        clips.extend(struct.pack('<48sf4I', name.encode('ascii'), fps, 0, 0, len(frames), len(artwork)))
        for bitmap, x, y in artwork:
            geometry = (x/ppu, -(y+bitmap.height-.001)/ppu, (x+bitmap.width-.001)/ppu, -y/ppu)
            result[index] = (bitmap, geometry, name)
            frames.append(index)
            index += 1
    return result, clips, frames
