"""Source Animator curves for Monomon's intermediate mask room."""
import converted_files
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
SNAPSHOT = ROOT/'_private/minimum-bosses/monomon-mask/animations.json'


def streamed_channels(clip):
    stream = clip['m_MuscleClip']['m_Clip']['data']['m_StreamedClip']
    raw = struct.pack(f'<{len(stream["data"])}I', *stream['data'])
    channels = [[] for _ in range(stream['curveCount'])]
    offset = 0
    while offset < len(raw):
        time, count = struct.unpack_from('<fI', raw, offset)
        offset += 8
        for _ in range(count):
            index, *coefficients = struct.unpack_from('<I4f', raw, offset)
            offset += 20
            # Native streamed PPtr curves follow curveCount float channels.
            while index >= len(channels):
                channels.append([])
            channels[index].append([max(0., time), *coefficients])
    return [[row for i, row in enumerate(rows) if i+1 == len(rows) or row[0] != rows[i+1][0]]
            for rows in channels]


def extract():
    sys.path.insert(0, str(ROOT/'_private/tools/boss-python'))
    import UnityPy
    path = ROOT/'_private/original/Hollow Knight/hollow_knight_Data/sharedassets403.assets'
    env = UnityPy.load(str(path))
    clips = {str(o.path_id): o.read_typetree() for o in env.objects if o.path_id in (27, 29)}
    SNAPSHOT.parent.mkdir(parents=True, exist_ok=True)
    SNAPSHOT.write_text(json.dumps(dict(build=22529139, source_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                                       clips=clips), indent=2)+'\n', encoding='utf-8')
    sprites = {}
    for ref in clips['29']['m_ClipBindingConstant']['pptrCurveMapping']:
        ident = ref['m_PathID']
        if not ident or str(ident) in sprites:
            continue
        if ref['m_FileID']:
            raise ValueError('Mask clip now references external sprites')
        sprite = next(o for o in env.objects if o.path_id == ident).read()
        image = sprite.image.convert('RGBA')
        image.save(SNAPSHOT.parent/f'{ident}.png')
        rect, pivot, ppu = sprite.m_Rect, sprite.m_Pivot, sprite.m_PixelsToUnits
        sprites[str(ident)] = dict(geometry=[-pivot.x*rect.width/ppu, -pivot.y*rect.height/ppu,
            (1-pivot.x)*rect.width/ppu, (1-pivot.y)*rect.height/ppu], size=list(image.size))
    (SNAPSHOT.parent/'sprites.json').write_text(json.dumps(sprites, indent=2)+'\n', encoding='utf-8')


OWNER = 0x6d000000


def add_tiles(tiles, by_sprite):
    """Private mask sprites share the room atlas, depth and material pipeline."""
    from PIL import Image
    sprites = json.loads((SNAPSHOT.parent/'sprites.json').read_text())
    clip = json.loads(SNAPSHOT.read_text())['clips']['29']
    refs = clip['m_ClipBindingConstant']['pptrCurveMapping']
    for i, ref in enumerate(refs):
        ident = str(ref['m_PathID'])
        if ident == '0':
            continue
        key = f'monomon-mask-{i}'
        image = Image.open(SNAPSHOT.parent/f'{ident}.png').convert('RGBA')
        left, bottom, right, top = sprites[ident]['geometry']
        by_sprite[key] = []
        for y in range(0, image.height, 512):
            for x in range(0, image.width, 512):
                bitmap = image.crop((x, y, min(x+512, image.width), min(y+512, image.height)))
                if not bitmap.getchannel('A').getbbox():
                    continue
                index = len(tiles)
                by_sprite[key].append(index)
                geometry = (left+x*(right-left)/image.width, top-(y+bitmap.height)*(top-bottom)/image.height,
                            left+(x+bitmap.width)*(right-left)/image.width, top-y*(top-bottom)/image.height)
                tiles.append(dict(bitmap=bitmap, geometry=geometry, source=key, index=index))
    # Bound raw sprite pixels; the resident packer separately enforces the room GPU budget.
    if sum(t['bitmap'].width*t['bitmap'].height*4 for t in tiles if t['source'].startswith('monomon-mask-')) > 8*1024*1024:
        raise ValueError('Mask animation exceeds its 8 MiB raw-pixel limit')


def add_draws(draws, by_sprite):
    source = json.loads((ROOT/'_private/minimum-bosses/uumuu/level403-source.json').read_text())
    transforms = {c['path_id']: c['data'] for a in source['actors'] for c in a['components'] if c['type'] == 'Transform'}
    from convert_room_visuals import project
    for key, indices in by_sprite.items():
        if not key.startswith('monomon-mask-'):
            continue
        i = int(key.rsplit('-', 1)[1])
        actor = next(a for a in source['actors'] if a['path_id'] == (18 if i < 24 else 26))
        transform = next(c['data'] for c in actor['components'] if c['type'] == 'Transform')
        chain = [transform]
        while transform['m_Father']['m_PathID']:
            transform = transforms[transform['m_Father']['m_PathID']]
            chain.append(transform)
        chain = [{field: {'m_'+axis.upper(): value for axis, value in t[field].items()}
                  for field in ('m_LocalScale', 'm_LocalRotation', 'm_LocalPosition')} for t in chain]
        origin = project((0, 0, 0), chain)
        px, py = project((1, 0, 0), chain), project((0, 1, 0), chain)
        r = next(c['data'] for c in actor['components'] if c['type'] == 'SpriteRenderer')
        values = [px[0]-origin[0], py[0]-origin[0], origin[0], px[1]-origin[1], py[1]-origin[1], origin[1], origin[2],
                  r['m_Color']['r'], r['m_Color']['g'], r['m_Color']['b'], r['m_Color']['a'], r['m_SortingLayer'], r['m_SortingOrder'], 0, 0, 0]
        for index in indices:
            draws.append(dict(sprite=index, owner=OWNER+i, values=values.copy(), sort=(values[11], values[12], -values[6], OWNER+i, index)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--extract', action='store_true')
    args = parser.parse_args()
    if args.extract:
        extract()
    clip = json.loads(SNAPSHOT.read_text(encoding='utf-8'))['clips']['27']
    if clip['m_Name'] != 'Boss_Door_Cutscene':
        raise ValueError('Changed mask-room Animator clip')
    channels = streamed_channels(clip)
    lines = ['#pragma once', '// Generated: build22529139 sharedassets403 Animator32 / clip27.',
             '#include <algorithm>', 'namespace hk3ds::monomon_mask_data {',
             'struct Key {float time,a,b,c,d;};',
             'template<unsigned N> inline float sample(const Key (&keys)[N],float time){',
             '    const Key* key=&keys[0];',
             '    for(const auto& next:keys){if(next.time>time)break;key=&next;}',
             '    const float t=std::max(0.f,time-key->time);',
             '    return ((key->a*t+key->b)*t+key->c)*t+key->d;', '}']
    for name, index in (('rootX', 0), ('rootY', 1), ('rootZ', 2), ('faderAlpha', 7), ('whiteAlpha', 8), ('blackMaskAlpha', 9)):
        lines.append(f'inline constexpr Key {name}[] = {{')
        lines.extend('    {'+','.join(repr(float(v))+'f' for v in row)+'},' for row in channels[index])
        lines.append('};')
    mask = json.loads(SNAPSHOT.read_text(encoding='utf-8'))['clips']['29']
    mask_channels = streamed_channels(mask)
    for name, index in (('bodySprite', 19), ('crackSprite', 20), ('crackActive', 12), ('crackX', 3), ('crackY', 4),
                        ('crackScaleX', 6), ('crackScaleY', 7)):
        lines.append(f'inline constexpr Key {name}[] = {{')
        lines.extend('    {'+','.join(repr(float(v))+'f' for v in row)+'},' for row in mask_channels[index])
        lines.append('};')
    lines.append(f'inline constexpr unsigned ownerBase={OWNER};')
    actors = json.loads((ROOT/'_private/minimum-bosses/uumuu/level403-source.json').read_text(encoding='utf-8'))['actors']
    transforms = {c['path_id']: (a['path_id'], c['data']['m_Father']['m_PathID'])
                  for a in actors for c in a['components'] if c['type'] == 'Transform'}
    children = []
    for owner, parent in transforms.values():
        while parent:
            ancestor, parent = transforms[parent]
            if ancestor == 6:
                children.append(owner)
                break
    lines.append('inline constexpr unsigned children[] = {'+','.join(map(str, sorted(children)))+'};')
    lines.extend(['}', ''])
    (ROOT/'include/monomon_mask_data.hpp').write_text('\n'.join(lines), encoding='utf-8', newline='\n')
    print('Monomon mask: original Animator curves and sprite bindings')


if __name__ == '__main__':
    main()
