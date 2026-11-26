"""Append source resources 14766/15092 blood to the scene-local Uumuu bank."""
import json
import struct
from pathlib import Path
from PIL import Image
import tex3ds_cache
from false_knight_effects import interval, samples, alpha_samples, texture_tiles


def append_blood(romfs, source, tex3ds, header, corpse):
    source = Path(source)
    records = json.loads((source / 'components.json').read_text())
    system, renderer = records['14766'], records['15092']
    p, render = system['data'], renderer['data']
    initial = p['InitialModule']
    shape = p['ShapeModule']
    if not shape['enabled'] or shape['type'] != 10 or shape['radiusThickness'] != 1:
        raise ValueError('Expected the source filled-circle blood emitter')
    color = initial['startColor']['maxColor']
    material = dict(renderer['material']['m_SavedProperties']['m_Colors'])
    tint = material.get('_Color', dict(r=1, g=1, b=1, a=1))
    texture = Image.open(source / renderer['texture']).convert('RGBA')
    texture = Image.merge('RGBA', tuple(channel.point(
        [round(v * color[key] * tint[key]) for v in range(256)])
        for channel, key in zip(texture.split(), 'rgba')))
    tiles = list(texture_tiles(texture, p['UVModule']))
    if len(tiles) != 3 or any(t.size != (64, 64) for t in tiles):
        raise ValueError('Source blood sheet changed')
    bank = romfs / 'uumuu-effects-art.bin'
    raw = bank.read_bytes()
    magic, pages, sprites, clips, frames = struct.unpack_from('<4s4I', raw)
    if magic != b'HKA1' or pages != 1:
        raise ValueError('Expected the unmodified Uumuu lightning bank')
    sprite_end = 20 + sprites * 36
    clip_end = sprite_end + clips * 68
    atlas = Image.new('RGBA', (128, 128))
    added = []
    for i, tile in enumerate(tiles):
        x, y = i % 2 * 64, i // 2 * 64
        atlas.paste(tile, (x, y))
        added.append(struct.pack('<5I4f', pages, x, y, 64, 64, -.5, -.5, .5, .5))
    # A static selected tile still needs a valid positive HKA1 clip rate.
    clip = struct.pack('<48sf4I', b'Blood', 1, 0, 0, frames, len(tiles))
    bank.write_bytes(struct.pack('<4s4I', magic, pages + 1, sprites + 3, clips + 1, frames + 3)
                     + raw[20:sprite_end] + b''.join(added)
                     + raw[sprite_end:clip_end] + clip + raw[clip_end:]
                     + struct.pack('<3I', sprites, sprites + 1, sprites + 2))
    png = romfs.parent / 'uumuu-blood-page.png'
    atlas.save(png)
    tex3ds_cache.run([str(tex3ds), '-f', 'rgba4', '-z', 'lz11', '-o',
                     str(romfs / f'uumuu-effects-page-{pages}.t3x'), str(png)],
                    check=True, capture_output=True)
    def velocity(axis):
        return interval(p['VelocityModule'][axis]) if p['VelocityModule']['enabled'] else [0, 0]
    force = interval(p['ForceModule']['y']) if p['ForceModule']['enabled'] else [0, 0]
    values = dict(life=interval(initial['startLifetime']), speed=interval(initial['startSpeed']),
                  size=interval(initial['startSize']), rotation=interval(initial['startRotation']),
                  velocityY=velocity('y'), forceY=force, sizeCurve=samples(p['SizeModule']),
                  alphaCurve=alpha_samples(p['ColorModule']))
    scalars = dict(shapeRadius=shape['radius']['value'],
                   speedLimit=p['ClampVelocityModule']['magnitude']['scalar'],
                   scale=system['transform']['m_LocalScale']['y'],
                   lengthScale=render['m_LengthScale'], velocityScale=render['m_VelocityScale'],
                   pivotY=render['m_Pivot']['y'])
    rows = ['// Generated from build22529139 resources.assets particles 14766/renderer15092.',
            '#pragma once', 'namespace hk3ds::uumuu_blood_data {']
    for name, value in values.items():
        rows.append('inline constexpr float ' + name + '[]={' + ','.join(f'{v:.9f}f' for v in value) + '};')
    for name, value in scalars.items():
        rows.append(f'inline constexpr float {name}={value:.9f}f;')
    corpse = json.loads(Path(corpse).read_text())
    fsm = next(c['decoded'] for c in corpse['actors'][0]['components'] if c['type'] == 'PlayMakerFSM')
    for name in ('Init', 'Steam', 'Blow'):
        state = next(s for s in fsm['states'] if s['name'] == name)
        action = next(a for a in state['actions'] if a['type'] in ('SpawnBlood', 'SpawnBloodTime'))
        params = action['parameters']
        value = lambda key: params[key]['value']
        position = value('position')
        values = [position['x'], position['y'], value('delay') if 'delay' in params else 0,
                  *[value(k) for k in ('spawnMin', 'spawnMax', 'speedMin', 'speedMax', 'angleMin', 'angleMax')]]
        rows.append('inline constexpr float ' + name.lower() + '[]={' + ','.join(f'{v:.9f}f' for v in values) + '};')
    Path(header).write_text('\n'.join(rows + ['}', '']), newline='\n')
    return 128 * 128 * 2
