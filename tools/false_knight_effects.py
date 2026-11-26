"""Append copied False Knight smoke/debris artwork and instance settings to HKA1."""
import json
import math
import struct
from pathlib import Path
from PIL import Image
import tex3ds_cache

SYSTEMS = (178, 187, 188, 189, 190, 14766, 14776, 14655)
RENDERERS = (194, 203, 204, 206, 205, 15092, 15096, 14824)


def texture_tiles(texture, module):
    cols, rows = (int(module['tilesX']), int(module['tilesY'])) if module['enabled'] else (1, 1)
    if cols < 1 or rows < 1 or texture.width % cols or texture.height % rows:
        raise ValueError('Invalid source particle sheet')
    w, h = texture.width//cols, texture.height//rows
    for y in range(rows):
        for x in range(cols):
            yield texture.crop((x*w, y*h, (x+1)*w, (y+1)*h))


def interval(value):
    state = value['minMaxState']
    if state == 0:
        return [value['scalar']] * 2
    if state == 3:
        return [value['minScalar'], value['scalar']]
    raise ValueError(f'Unsupported birth range {state}')


def curve(keys, t):
    if not keys:
        return 1.
    if t <= keys[0]['time']:
        return keys[0]['value']
    for a, b in zip(keys, keys[1:]):
        if t <= b['time']:
            dt = b['time'] - a['time']
            u = (t-a['time'])/dt
            return ((2*u**3-3*u*u+1)*a['value']+(u**3-2*u*u+u)*dt*a['outSlope']
                    +(-2*u**3+3*u*u)*b['value']+(u**3-u*u)*dt*b['inSlope'])
    return keys[-1]['value']


def samples(module):
    if not module['enabled']:
        return [1.] * 8
    c = module['curve']
    if c['minMaxState'] == 0:
        return [c['scalar']] * 8
    if c['minMaxState'] not in (1, 2):
        raise ValueError('Unsupported size curve')
    high = [c['scalar']*curve(c['maxCurve']['m_Curve'], i/7) for i in range(8)]
    if c['minMaxState'] == 2:
        # approximation: eight mean samples of the two authored size curves.
        return [(v+c['minScalar']*curve(c['minCurve']['m_Curve'], i/7))*.5 for i,v in enumerate(high)]
    return high


def alpha_samples(module):
    if not module['enabled']:
        return [1.] * 8
    gradient = module['gradient']
    if gradient['minMaxState'] not in (1, 3):
        raise ValueError('Unsupported alpha gradient')
    g = gradient['maxGradient']
    keys = [(g[f'atime{i}']/65535., g[f'key{i}']['a']) for i in range(g['m_NumAlphaKeys'])]
    result = []
    for i in range(8):
        t = i/7
        value = keys[-1][1]
        for (ta, a), (tb, b) in zip(keys, keys[1:]):
            if t <= tb:
                value = a+(b-a)*max(0., (t-ta)/max(.0001, tb-ta))
                break
        result.append(value)
    if gradient['minMaxState'] == 3:
        other = dict(module, gradient=dict(gradient, minMaxState=1, maxGradient=gradient['minGradient']))
        # approximation: retain eight mean alpha samples, as other port particles.
        result = [(a+b)*.5 for a,b in zip(result, alpha_samples(other))]
    return result


def append_effects(romfs, source, tex3ds):
    source = Path(source)
    records = json.loads((source/'components.json').read_text())
    path = romfs/'false-knight-art.bin'
    raw = path.read_bytes()
    _, pages, sprite_count, clip_count, frame_count = struct.unpack_from('<4s4I', raw)
    sprites = raw[20:20+sprite_count*36]
    start = 20+sprite_count*36
    clips = raw[start:start+clip_count*68]
    frames = raw[start+clip_count*68:]
    atlas = Image.new('RGBA', (512, 512))
    added_sprites, added_clips, added_frames, configs = [], [], [], []
    x = y = row = 0
    for index, (ps, renderer) in enumerate(zip(SYSTEMS, RENDERERS)):
        record = records[str(ps)]
        p = record['data']
        texture = Image.open(source/records[str(renderer)]['texture']).convert('RGBA')
        color = p['InitialModule']['startColor']['maxColor']
        if index == 6:
            # Blood's collision sub-emitter explicitly inherits the parent's color.
            color = records[str(SYSTEMS[5])]['data']['InitialModule']['startColor']['maxColor']
        material = dict(records[str(renderer)]['material']['m_SavedProperties']['m_Colors'])
        tint = material.get('_Color', dict(r=1., g=1., b=1., a=1.))
        texture = Image.merge('RGBA', tuple(channel.point([round(v*color[key]*tint[key]) for v in range(256)])
                              for channel, key in zip(texture.split(), 'rgba')))
        sheet = list(texture_tiles(texture, p['UVModule']))
        tiles = len(sheet)
        first = sprite_count+len(added_sprites)
        for tile, bitmap in enumerate(sheet):
            w, h = bitmap.size
            if x+w+2 > atlas.width:
                x, y, row = 0, y+row, 0
            if y+h+2 > atlas.height:
                raise ValueError('False Knight effects exceed the bounded effects atlas')
            atlas.paste(bitmap, (x+1, y+1))
            added_sprites.append(struct.pack('<5I4f', pages, x+1, y+1, w, h, -.5*w/h, -.5, .5*w/h, .5))
            added_frames.append(first+tile)
            x += w+2
            row = max(row, h+2)
        added_clips.append(struct.pack('<48sf4I', f'FX/{index}'.encode(), tiles, 0, 0,
                                       frame_count+len(added_frames)-tiles, tiles))
        initial, shape = p['InitialModule'], p['ShapeModule']
        transform = record['transform']
        pos, scale = transform['m_LocalPosition'], transform['m_LocalScale']
        q, shape_scale = transform['m_LocalRotation'], shape['m_Scale']
        # approximation: diagonal XY projection of the source emitter rotation.
        # Retain its broad box footprint and the nearly flat landing dust disk.
        projected_x = (1-2*(q['y']**2+q['z']**2))*scale['x']*shape_scale['x']
        projected_y = (1-2*(q['x']**2+q['z']**2))*scale['y']*shape_scale['y']
        velocity = p['VelocityModule']
        vx = interval(velocity['x']) if velocity['enabled'] else [0., 0.]
        vy = interval(velocity['y']) if velocity['enabled'] else [0., 0.]
        force = p['ForceModule']
        force_y = interval(force['y']) if force['enabled'] else [0., 0.]
        limit = p['ClampVelocityModule']
        speed_limit = limit['magnitude']['scalar'] if limit['enabled'] else 0.
        rate = p['EmissionModule']['rateOverTime']['scalar'] if p['EmissionModule']['enabled'] else 0.
        render = records[str(renderer)]['data']
        values = [p['lengthInSec'], rate,
                  *interval(initial['startLifetime']), *interval(initial['startSpeed']),
                  *interval(initial['startSize']), *interval(initial['startRotation']),
                  initial['gravityModifier']['scalar'], shape['radius']['value'], *vx, *vy,
                  pos['x'], pos['y'], scale['x'], scale['y'], shape['type'],
                  math.radians(shape['angle']), projected_x, projected_y,
                  *samples(p['SizeModule']), *alpha_samples(p['ColorModule']), *force_y, speed_limit,
                  p['scalingMode'], render['m_RenderMode'], render['m_LengthScale'],
                  render['m_VelocityScale'], render['m_Pivot']['x'], render['m_Pivot']['y']]
        if (len(values) != 49 or not all(math.isfinite(v) for v in values)
                or p['scalingMode'] not in (0, 1, 2) or render['m_RenderMode'] not in (0, 1)):
            raise ValueError('Invalid False Knight effect config')
        configs.append(struct.pack('<49f', *values))
    path.write_bytes(struct.pack('<4s4I', b'HKA1', pages+1, sprite_count+len(added_sprites),
                                clip_count+len(SYSTEMS), frame_count+len(added_frames))
                     +sprites+b''.join(added_sprites)+clips+b''.join(added_clips)
                     +frames+struct.pack(f'<{len(added_frames)}I', *added_frames))
    png = romfs.parent/'false-knight-fx-page.png'
    atlas.save(png)
    tex3ds_cache.run([str(tex3ds), '-f', 'rgba4', '-z', 'lz11', '-o',
                     str(romfs/f'false-knight-page-{pages}.t3x'), str(png)], check=True, capture_output=True)
    (romfs/'false-knight-fx.bin').write_bytes(b'FKF4'+b''.join(configs))
    return atlas.width*atlas.height*2
