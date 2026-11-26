"""Convert the four visible Teacher's Archives tank emitters to PSP HAP1."""
import math
import struct
import sys

SCENE = 'Fungus3_archive_02'
BANK = 'archive-particles.bin'
EMITTERS = ((7654, 4842, 7862), (7656, 4828, 7863),
            (7681, 5254, 7884), (7723, 6216, 7926))
TEXTURES = (('resources.assets', 341), ('sharedassets50.assets', 10))


def inputs(source):
    data = source / '_private/original/Hollow Knight/hollow_knight_Data'
    return [data / name for name in ('level228', *(item for name, _ in TEXTURES
                                                   for item in (name, name + '.resS')))]


def samples(curve, sampler):
    def keys(track):
        return {'m_Curve': [{f'm_{key[0].upper()}{key[1:]}': value for key, value in point.items()}
                            for point in track['m_Curve']]}
    return sampler({'m_' + key[0].upper() + key[1:]: keys(value) if key.endswith('Curve') else value
                    for key, value in curve.items()})


def gradient_samples(gradient, sampler):
    return sampler({key if key.startswith('m_') else 'm_' + key[0].upper() + key[1:]:
                    dict(zip(('m_R', 'm_G', 'm_B', 'm_A'), (value[c] for c in 'rgba')))
                    if key.startswith('key') else value for key, value in gradient.items()})


def encode_bank(rows, total, first_sprite):
    return struct.pack('<4s5I', b'HAP1', 5, len(rows), total, first_sprite, 0) + b''.join(rows)


def compile_archive(source, first_sprite):
    sys.path.insert(0, str(source / '_private/tools/boss-python'))
    from UnityPy import Environment
    from convert_ambient_particles import ROW, SAMPLES, _gradient_samples, _minmax_samples, _range

    level = inputs(source)[0]
    def objects(path, stream=False):
        env = Environment()
        if stream:
            env.load_file(str(path) + '.resS')
        return env.load_file(str(path)).objects

    original = objects(level)
    def read(pid):
        return original[pid].read_typetree(check_read=False)

    def vec(value, axes='xyz'):
        return [value[axis] for axis in axes]

    sprites = [objects(level.with_name(name), stream=True)[pid].read().image.convert('RGBA')
               for name, pid in TEXTURES]
    rows = []
    total = 0
    parent = read(4841)
    if (vec(parent['m_LocalRotation'], 'xyzw') != [0, 0, 0, 1] or
            vec(parent['m_LocalPosition']) != [0, 0, 0] or
            vec(parent['m_LocalScale']) != [1, 1, 1] or parent['m_Father']['m_PathID'] != 4838):
        raise ValueError('Changed Archives tank parent transform')
    tank = read(4838)
    if vec(tank['m_LocalRotation'], 'xyzw') != [0, 0, 0, 1] or vec(tank['m_LocalScale']) != [1, 1, 1]:
        raise ValueError('Changed Archives tank transform')
    tank_position = vec(tank['m_LocalPosition'])
    def bounds(curve):
        return sorted(samples(curve, _range))

    for system_id, transform_id, renderer_id in EMITTERS:
        system, transform, renderer = map(read, (system_id, transform_id, renderer_id))
        init, shape = system['InitialModule'], system['ShapeModule']
        color, size, velocity = system['ColorModule'], system['SizeModule'], system['VelocityModule']
        if shape['type'] not in (5, 10) or transform['m_Father']['m_PathID'] != 4841:
            raise ValueError(f'Changed Archives emitter {system_id}')
        scale = vec(transform['m_LocalScale'])
        # Approximate the Soul hemisphere with a bounded volume; the bubbles use authored boxes.
        half = ([abs(axis) * shape['radius']['value'] for axis in scale] if shape['type'] == 10 else
                [abs(a * b) * .5 for a, b in zip(scale, vec(shape['m_Scale']))])
        colors = (gradient_samples(color['gradient']['maxGradient'], _gradient_samples)
                  if color['enabled'] else [(1, 1, 1, 1)] * SAMPLES)
        sizes = (samples(size['curve'], _minmax_samples) if size['enabled'] else [1.] * SAMPLES)
        start = init['startColor']
        max_color = vec(start['maxColor'], 'rgba')
        min_color = vec(start['minColor'], 'rgba') if start['minMaxState'] == 2 else max_color
        flags = 8 | 128 | 256 | int(color['enabled']) | (int(size['enabled']) << 1)
        flags |= (4 if start['minMaxState'] == 2 else 0) | (64 if velocity['inWorldSpace'] else 0)
        count = init['maxNumParticles']
        position = [a+b for a, b in zip(tank_position, vec(transform['m_LocalPosition']))]
        rate = bounds(system['EmissionModule']['rateOverTime'])
        life = bounds(init['startLifetime'])
        count = min(count, math.ceil(rate[1] * life[1]) + 1)
        total += count
        velocity_bounds = [v for axis in 'xyz' for v in (bounds(velocity[axis]) if velocity['enabled'] else (0, 0))]
        row = ROW.pack(count, renderer['m_SortingLayer'], renderer['m_SortingOrder'], flags,
                       *position, *vec(transform['m_LocalRotation'], 'xyzw'), *scale,
                       half[0], half[2], 0, *rate, *life,
                       *bounds(init['startSize']), *bounds(init['startSpeed']), *bounds(init['gravityModifier']),
                       *velocity_bounds, *min_color, *max_color, *(v for rgba in colors for v in rgba),
                       *sizes, *bounds(init['startRotation']), *sizes)
        sprite = first_sprite + int(shape['type'] == 5)
        rows.append(struct.pack('<I', sprite) + row + struct.pack('<2fIf', 0, 0, 1, half[1]))
    if total > 512:
        raise ValueError(f'Archives tank emitters exceed runtime capacity: {total}')
    return sprites, encode_bank(rows, total, first_sprite)
