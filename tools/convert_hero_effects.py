"""Compile original hero emitters into the existing hero atlas and a fixed bank."""
import converted_files  # Install hardlink copy-on-write before asset writes.
import copy
import hashlib
import json
import math
import struct
from PIL import Image
from convert_room_visuals import project

OWNERS = (4217, 6465, 7661, 8103, 8243, 3992)
FLOATS = 86


def damage_flash_curves(source):
    manifest = json.loads((source / 'damage-flash.json').read_text())
    clip = json.loads((source / manifest['animation']['clip']).read_text())
    if (clip['m_Name'] != 'hit_crack_simple' or len(clip['m_ScaleCurves']) != 1 or
            len(clip['m_FloatCurves']) != 1):
        raise ValueError('Unsupported damage flash animation')
    scale, alpha = clip['m_ScaleCurves'][0], clip['m_FloatCurves'][0]
    if scale['m_Path'] or alpha['m_Path'] or alpha['m_Attribute'] != 'm_Color.a':
        raise ValueError('Unsupported damage flash animation bindings')
    values = []
    for curve, axis in ((scale, 'm_X'), (scale, 'm_Y'), (alpha, None)):
        keys = curve['m_Curve']['m_Curve']
        if len(keys) != 3 or any(k['m_WeightedMode'] for k in keys):
            raise ValueError('Unsupported damage flash keys')
        for i, key in enumerate(keys):
            value = lambda field: key[field][axis] if axis else key[field]
            incoming, outgoing = value('m_InSlope'), value('m_OutSlope')
            if not math.isfinite(outgoing):
                # The authored alpha hold is a constant step between equal values.
                if i + 1 >= len(keys) or value('m_Value') != keys[i+1]['m_Value']:
                    raise ValueError('Unsupported nonconstant flash step')
                outgoing = 0
            if not math.isfinite(incoming):
                raise ValueError('Unsupported flash incoming tangent')
            values.extend((key['m_Time'], value('m_Value'), incoming, outgoing))
    return values


def damage_flash(source):
    from convert_room_visuals import decode
    manifest = json.loads((source / 'damage-flash.json').read_text())
    for name, digest in manifest['source_sha256'].items():
        if hashlib.sha256((source / name).read_bytes()).hexdigest() != digest:
            raise ValueError('Changed damage flash source: ' + name)
    read = lambda kind: json.loads((source / manifest['components'][kind][0]).read_text())
    transform, delay, rotation = read('Transform'), read('DeactivateAfterDelay'), read('RandomRotationRange')
    if not delay['m_Structure']['stayInPlace'] or delay['m_Structure']['isRealtime']:
        raise ValueError('Unsupported damage flash attachment/clock')
    renderer = read('SpriteRenderer')
    if any(renderer['m_Color']['m_'+c] != 1 for c in 'RGBA'):
        raise ValueError('Unsupported damage flash tint')
    bitmap, bounds = decode(Image.open(source / manifest['image']).convert('RGBA'),
                            json.loads((source / manifest['sprite']).read_text()))
    values = [transform['m_LocalPosition']['m_'+c] for c in 'XY']
    values += [transform['m_LocalScale']['m_'+c] for c in 'XY']
    values += [delay['m_Structure']['time']]
    values += [math.radians(rotation['m_Structure'][c]) for c in ('min', 'max')]
    clip = dict(name='Hero Damage Flash', fps=1, wrapMode=6, loopStart=0,
                frames=[dict(spriteCollection={'m_PathID': -3}, spriteId=0)])
    return ((-3, 0), bitmap, bounds), clip, values, [source / n for n in manifest['source_sha256']]


def damage_flings(source):
    from convert_hero import decode_sprite
    manifest = json.loads((source / 'damage-flings.json').read_text())
    for name, digest in manifest['source_sha256'].items():
        if hashlib.sha256((source / name).read_bytes()).hexdigest() != digest:
            raise ValueError('Changed damage fling source: ' + name)
    read = lambda name: json.loads((source / name).read_text())
    actions = [a['parameters'] for state in read('dispatch.json')['states'] if state['name'] == 'Gen'
               for a in state['actions'] if a['enabled'] and a['type'].endswith('.FlingObjectsFromGlobalPool')]
    if actions != manifest['actions'] or len(actions) != 4:
        raise ValueError('Changed damage fling dispatch')
    nodes = {node['owner']: node for node in manifest['nodes']}
    sprites, clips, records = [], [], []
    for owner, node in nodes.items():
        parts = node['components']
        component = lambda kind: read(parts[kind][0])
        animator = component('tk2dSpriteAnimator')['m_Structure']
        body = component('Rigidbody2D')
        facing = component('FaceAngleSimple')['m_Structure']
        recycle = component('RecycleAfter2dtkAnimation')['m_Structure']
        scale = component('Transform')['m_LocalScale']
        tint = component('tk2dSprite')['m_Structure']['_color']
        if (animator['isRealtime'] or body['m_GravityScale'] or body['m_LinearDamping'] or
                not facing['everyFrame'] or facing['angleOffset'] or recycle['randomiseRotation'] or
                scale['m_X'] != scale['m_Y'] or node['clip']['wrapMode'] != 2):
            raise ValueError('Unsupported damage fling motion/animation')
        for i, frame in enumerate(node['clip']['frames']):
            key = 'resources.assets-' + str(frame['spriteCollection']['m_PathID']) + '.json'
            collection = read(key)['m_Structure']
            atlas = Image.open(source / node['images'][key]).convert('RGBA')
            bitmap, bounds = decode_sprite(atlas, collection['spriteDefinitions'][frame['spriteId']])
            bitmap = Image.merge('RGBA', tuple(channel.point(lambda v, f=tint[c]: round(v*f))
                for channel, c in zip(bitmap.split(), 'rgba')))
            identity = (-4, len(sprites))
            sprites.append((identity, bitmap, bounds))
            clips.append(dict(name=f'Hero FX {owner} {i}', fps=1, wrapMode=6, loopStart=0,
                frames=[dict(spriteCollection={'m_PathID': identity[0]}, spriteId=identity[1])]))
    for action in manifest['actions']:
        owner = action['gameObject']['value']['m_PathID']
        node = nodes[owner]
        scale = read(node['components']['Transform'][0])['m_LocalScale']['m_X']
        if any(action[k]['value'] for k in ('originVariationX', 'originVariationY')):
            raise ValueError('Unsupported damage fling origin variation')
        values = [0.] * FLOATS
        duration = len(node['clip']['frames']) / node['clip']['fps']
        values[0] = values[2] = values[3] = duration
        values[4:10] = [action['speedMin']['value'], action['speedMax']['value'], scale, scale,
                       math.radians(action['angleMin']['value']), math.radians(action['angleMax']['value'])]
        values[49] = node['clip']['fps']
        values[50:74] = [1.] * 24
        values[82:84] = [action['spawnMin']['value'], action['spawnMax']['value']]
        previous = next((r for r in records if r['owner'] == owner), None)
        if previous is None:
            records.append(dict(owner=owner, shape=0, tiles=len(node['clip']['frames']), flags=4, values=values,
                                sorting=[read(key)['m_SortingOrder'] for key in node['renderers']]))
        else:
            if any(values[i] != previous['values'][i] for i in range(FLOATS) if i not in (8, 9)):
                raise ValueError('Unsupported asymmetric damage fling batches')
            previous['values'][84:86] = values[8:10]
    return sprites, clips, records, [source / name for name in manifest['source_sha256']]


def compile_effects(source, output):
    from convert_decor_effects import limits, particle_alpha, sprite_material_color, scene_ambient
    manifest = json.loads((source / 'hero-effects.json').read_text())
    for name, digest in manifest['source_sha256'].items():
        if hashlib.sha256((source / name).read_bytes()).hexdigest() != digest:
            raise ValueError('Changed hero effect source: ' + name)
    read = lambda name: json.loads((source / name).read_text())
    sprites, clips, configs = [], [], []
    images = {}
    binary = bytearray(struct.pack('<4sI', b'HFX4', len(OWNERS)+2))
    for node, owner in zip(manifest['nodes'], OWNERS):
        if node['owner'] != owner:
            raise ValueError('Hero effect owner order changed')
        p = read(node['components']['ParticleSystem'][0])
        initial, shape = p['m_InitialModule'], p['m_ShapeModule']
        if p['m_Looping'] or p['m_EmissionModule']['m_Bursts'] or p['m_MoveWithTransform'] != 1:
            raise ValueError('Unsupported hero emitter admission')
        if shape['m_Type'] not in (4, 5, 10, 12) or p['m_ScalingMode'] != 2:
            raise ValueError('Unsupported hero emitter shape/scaling')
        transforms = [read(t) for t in node['transforms']]
        # Spawn sets the root position; preserve its authored rotation/scale.
        transforms[-1]['m_LocalPosition'] = dict(m_X=0, m_Y=0, m_Z=0)
        origin = project((0, 0, 0), transforms)
        rotation = copy.deepcopy(transforms)
        for tr in rotation:
            tr['m_LocalScale'] = dict(m_X=1, m_Y=1, m_Z=1)
            tr['m_LocalPosition'] = dict(m_X=0, m_Y=0, m_Z=0)
        axes, directions = [], []
        for axis in range(3):
            point = [0, 0, 0]
            point[axis] = 1
            axes.extend(a-b for a, b in zip(project(point, transforms), origin))
            directions.extend(project(point, rotation))
        def samples(module, field, default, side):
            return [limits(p[module][field], i/7)[side] if p[module]['m_Enabled'] else default for i in range(8)]
        force, velocity, clamp = p['m_ForceModule'], p['m_VelocityModule'], p['m_ClampVelocityModule']
        values = [p['m_LengthInSec'], limits(p['m_StartDelay_MinMaxCurve'])[1]]
        for field in ('m_StartLifetime', 'm_StartSpeed', 'm_StartSize', 'm_StartRotation'):
            values.extend(limits(initial[field]))
        values += [limits(initial['m_GravityModifier'])[1], shape['m_RadiusParameter']['m_Value'],
                   math.tan(math.radians(shape['m_Angle'])), math.radians(shape['m_ArcParameter']['m_Value']),
                   clamp['m_Dampen'] if clamp['m_Enabled'] else 0,
                   limits(clamp['m_Magnitude'])[1] if clamp['m_Enabled'] else 1e6,
                   *origin[:2], *axes, *directions]
        for module in (force, velocity):
            for axis in 'XYZ':
                values.extend(limits(module['m_'+axis]) if module['m_Enabled'] else [0, 0])
        values += limits(p['m_RotationModule']['m_Curve']) if p['m_RotationModule']['m_Enabled'] else [0, 0]
        values += samples('m_SizeModule', 'm_Curve', 1, 0) + samples('m_SizeModule', 'm_Curve', 1, 1)
        values += [particle_alpha(p['m_ColorModule'], i/7) for i in range(8)]
        values += [limits(p['m_EmissionModule']['m_RateOverTime'], i/7)[1] for i in range(8)]
        values += [shape['m_Scale']['m_'+axis] for axis in 'XYZ']
        values += [shape['m_Length']]
        if len(values) != FLOATS:
            raise ValueError('Hero effect field count')
        material = read(node['material'])['m_SavedProperties']
        shader = read(node['shader'])
        if shader['m_ParsedForm']['m_Name'] == 'Sprites/Default':
            tint = [material['m_Colors']['_Color']['m_'+c] for c in 'RGBA']
        else:
            tint = sprite_material_color(material, shader, scene_ambient())
        color = initial['m_StartColor']['m_MaxColor']
        bitmap = Image.open(source / node['image']).convert('RGBA')
        channels = bitmap.split()
        bitmap = Image.merge('RGBA', tuple(ch.point(lambda v, f=tint[i]*color['m_'+'RGBA'[i]]: round(v*f))
                                          for i, ch in enumerate(channels)))
        uv = p['m_UVModule']
        columns, rows = (uv['m_TilesX'], uv['m_TilesY']) if uv['m_Enabled'] else (1, 1)
        for row in range(rows):
            for col in range(columns):
                image = bitmap.crop((col*bitmap.width//columns, row*bitmap.height//rows,
                                     (col+1)*bitmap.width//columns, (row+1)*bitmap.height//rows))
                identity = (image.size, image.tobytes())
                if identity not in images:
                    images[identity] = len(sprites)
                    sprites.append(((-2, len(sprites)), image, (-.5, -.5, .5, .5)))
                i = images[identity]
                clips.append(dict(name=f'Hero FX {owner} {row*columns+col}', fps=1, wrapMode=6, loopStart=0,
                                  frames=[dict(spriteCollection={'m_PathID': -2}, spriteId=i)]))
        flags = (0 if force['m_InWorldSpace'] else 1) | (0 if velocity['m_InWorldSpace'] else 2)
        binary.extend(struct.pack('<4I86f', owner, shape['m_Type'], columns*rows, flags, *values))
        configs.append(dict(owner=owner, values=values, shape=shape['m_Type'], flags=flags, tiles=columns*rows))
    flash, clip, values, flash_inputs = damage_flash(source.parent / 'hero-damage-flash')
    sprites.append(flash)
    clips.append(clip)
    fling_sprites, fling_clips, fling_configs, fling_inputs = damage_flings(source.parent / 'hero-damage-flings')
    sprites.extend(fling_sprites)
    clips.extend(fling_clips)
    for config in fling_configs:
        binary.extend(struct.pack('<4I86f', config['owner'], config['shape'], config['tiles'], config['flags'], *config['values']))
    configs.extend(fling_configs)
    binary.extend(struct.pack('<7f', *values))
    binary.extend(struct.pack('<36f', *damage_flash_curves(source.parent / 'hero-damage-flash')))
    output.mkdir(parents=True, exist_ok=True)
    (output / 'hero-effects.bin').write_bytes(binary)
    (output / 'hero-effects-manifest.json').write_text(json.dumps(dict(owners=configs,
        source_sha256=manifest['source_sha256'], approximation='Eight samples for curves; projected 3D motion; fixed pool.'), indent=2)+'\n')
    return sprites, clips, [source / name for name in manifest['source_sha256']] + flash_inputs + fling_inputs
