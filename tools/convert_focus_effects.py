"""Compile the original Focus effects into the hero atlas and FOC1 bank."""
import hashlib
import converted_files
import json
import math
import struct
from pathlib import Path

from PIL import Image, ImageChops
from convert_hero import decode_sprite


def compile_focus(source, output):
    source, output = Path(source), Path(output)
    manifest = json.loads((source / 'focus-effects.json').read_text(encoding='utf-8'))
    if manifest.get('build') != 22529139:
        raise ValueError('Unexpected Focus source build')
    for name, digest in manifest['source_sha256'].items():
        path = source / name
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise ValueError('Changed Focus source: ' + name)

    def read(file):
        return json.loads((source / file).read_text(encoding='utf-8'))

    objects = {obj['name']: obj for obj in manifest['objects']}
    for name in ('Lines Anim', 'Heal Anim', 'Dust L', 'Dust R', 'Focus Ring', 'White Flash R'):
        if name not in objects:
            raise ValueError('Missing Focus source object: ' + name)

    sprites, clips, inputs = [], [], [source / n for n in manifest['source_sha256']]
    clip_identity = {}

    def append_clip(name, source_clip, library, kind, tint=(1, 1, 1, 1)):
        frame_ids = []
        collections = {int(Path(item['file']).stem.rsplit('-', 1)[1]): item
                       for item in library['sprite_collections']}
        for frame in source_clip['frames']:
            collection_id = frame['spriteCollection']['m_PathID']
            if frame['spriteCollection']['m_FileID'] != 0 or collection_id not in collections:
                raise ValueError('Unsupported external Focus sprite collection: ' + name)
            collection_info = collections[collection_id]
            collection = read(collection_info['file'])['m_Structure']
            if len(collection['textures']) != 1 or collection['textures'][0]['m_FileID'] != 0:
                raise ValueError('Unsupported Focus sprite texture map')
            texture_id = collection['textures'][0]['m_PathID']
            texture_name = f'resources.assets-{texture_id}.png'
            if texture_name not in manifest['images']:
                raise ValueError('Missing Focus sprite atlas image: ' + texture_name)
            atlas = Image.open(source / texture_name).convert('RGBA')
            bitmap, bounds = decode_sprite(atlas, collection['spriteDefinitions'][frame['spriteId']])
            if tint != (1, 1, 1, 1):
                bitmap = Image.merge('RGBA', tuple(channel.point(lambda value, factor=tint[i]:
                    max(0, min(255, round(value * factor)))) for i, channel in enumerate(bitmap.split())))
            if kind < 3:
                # Lines Anim material 34/shader 499: UI/BlendModes/Screen.
                # Premultiplication lets the GPU implement alpha-weighted Screen
                # without Unity's scene grab pass. Original line clips have no fade.
                red, green, blue, alpha = bitmap.split()
                bitmap = Image.merge('RGBA', tuple(ImageChops.multiply(c, alpha)
                    for c in (red, green, blue)) + (alpha,))
            sprite_id = len(sprites)
            identity = (-7, sprite_id)
            sprites.append((identity, bitmap, bounds))
            frame_ids.append(dict(spriteCollection={'m_PathID': -7}, spriteId=sprite_id))
        clip = dict(name=name, fps=source_clip['fps'], wrapMode=source_clip['wrapMode'],
                    loopStart=source_clip['loopStart'], frames=frame_ids)
        clips.append(clip)
        clip_identity[name] = (kind, source_clip)

    lines = objects['Lines Anim']
    line_library = lines['libraries'][0]
    line_clips = {clip['name']: clip for clip in line_library['clips']}
    line_tint = tuple(read(lines['components']['tk2dSprite'][0])['m_Structure']['_color'][c]
                      for c in 'rgba')
    append_clip('Focus FX Gather', line_clips['Focus Effect'], line_library, 0, line_tint)
    append_clip('Focus FX Cancel', line_clips['Focus Effect End'], line_library, 1, line_tint)

    burst = objects['Heal Anim']
    burst_library = burst['libraries'][0]
    burst_clip = next(c for c in burst_library['clips'] if c['name'] == 'Burst Effect')
    burst_tint = tuple(read(burst['components']['tk2dSprite'][0])['m_Structure']['_color'][c]
                       for c in 'rgba')
    append_clip('Focus FX Heal', burst_clip, burst_library, 2, burst_tint)

    def append_unity_sprite(obj, name, kind):
        sprite = obj['sprites'][0]
        asset = read(sprite['file'])
        atlas = Image.open(source / sprite['image']).convert('RGBA')
        rect = asset['m_Rect']
        x, y, width, height = (round(rect[f'm_{axis}']) for axis in ('X', 'Y', 'Width', 'Height'))
        crop = atlas.crop((x, atlas.height - y - height, x + width, atlas.height - y))
        tint = tuple(sprite['color']['m_' + c] for c in 'RGBA')
        crop = Image.merge('RGBA', tuple(channel.point(lambda value, factor=tint[i]:
            max(0, min(255, round(value * factor)))) for i, channel in enumerate(crop.split())))
        ppu, pivot = asset['m_PixelsToUnits'], asset['m_Pivot']
        bounds = (-pivot['m_X'] * width / ppu, -pivot['m_Y'] * height / ppu,
                  (1 - pivot['m_X']) * width / ppu, (1 - pivot['m_Y']) * height / ppu)
        sprite_id = len(sprites)
        sprites.append(((-7, sprite_id), crop, bounds))
        clip = dict(name=name, fps=1, wrapMode=6, loopStart=0,
                    frames=[dict(spriteCollection={'m_PathID': -7}, spriteId=sprite_id)])
        clips.append(clip)
        clip_identity[name] = (kind, clip)

    ring = objects['Focus Ring']
    ring_transform = read(ring['transform'])
    append_unity_sprite(ring, 'Focus FX Glow', 3)
    flash = objects['White Flash R']
    flash_transform = read(flash['transform'])
    flash_fade = read(flash['components']['SimpleSpriteFade'][0])['m_Structure']
    if not flash_fade['fadeInOnStart'] or flash_fade['fadeInColor']['a'] != 0:
        raise ValueError('Unsupported Focus flash fade')
    append_unity_sprite(flash, 'Focus FX Flash', 4)

    # The original FSM sets both Dust emitters to 60/s while Focus is active.
    # Source ParticleSystems provide the remaining fixed ranges and art tint.
    emitters = []
    for obj_name, clip_name in (('Dust L', 'Focus FX Particle L'), ('Dust R', 'Focus FX Particle R')):
        obj = objects[obj_name]
        particle = read(obj['components']['ParticleSystem'][0])
        transform = read(obj['transform'])
        image_name = obj['images'][0]
        bitmap = Image.open(source / image_name).convert('RGBA')
        initial = particle['m_InitialModule']
        start_color = initial['m_StartColor']['m_MaxColor']
        material = read(obj['particle_materials'][0])['m_SavedProperties']
        material_color = material['m_Colors']['_Color']
        tint = tuple(start_color['m_' + c] * material_color['m_' + c] for c in 'RGBA')
        bitmap = Image.merge('RGBA', tuple(channel.point(lambda value, factor=tint[i]:
            max(0, min(255, round(value * factor)))) for i, channel in enumerate(bitmap.split())))
        identity = (-7, len(sprites))
        sprites.append((identity, bitmap, (-.5, -.5, .5, .5)))
        particle_clip = dict(name=clip_name, fps=1, wrapMode=6, loopStart=0,
                             frames=[dict(spriteCollection={'m_PathID': -7}, spriteId=identity[1])])
        clips.append(particle_clip)
        emission = particle['m_EmissionModule']['m_RateOverTime']
        lifetime = initial['m_StartLifetime']
        size = initial['m_StartSize']
        shape = particle['m_ShapeModule']
        rotation = transform['m_LocalRotation']
        angle = math.atan2(2 * rotation['m_W'] * rotation['m_Z'],
                           1 - 2 * rotation['m_Z'] * rotation['m_Z'])
        half_cone = math.radians(shape['m_Angle'] / 2)
        speed_min = initial['m_StartSpeed']['m_MinScalar']
        speed_max = initial['m_StartSpeed']['m_Scalar']
        local_vectors = [(speed * math.sin(cone), speed * math.cos(cone))
                         for speed in (speed_min, speed_max)
                         for cone in (half_cone * (2 * index / 16 - 1) for index in range(17))]
        world_vectors = [(x * math.cos(angle) - y * math.sin(angle),
                          x * math.sin(angle) + y * math.cos(angle)) for x, y in local_vectors]
        vx = [v[0] for v in world_vectors]
        vy = [v[1] for v in world_vectors]
        radius = shape['m_RadiusParameter']['m_Value']
        local_x = radius * abs(transform['m_LocalScale']['m_X'])
        local_y = radius * abs(transform['m_LocalScale']['m_Y'])
        spawn_x = 2 * math.hypot(local_x * math.cos(angle), local_y * math.sin(angle))
        spawn_y = 2 * math.hypot(local_x * math.sin(angle), local_y * math.cos(angle))
        # approximation: FOC1 stores independent world-axis bounds; source cone angle and
        # rotated ellipse are reduced to their bounding ranges. Original cone angle: m_Angle.
        gradient = particle['m_ColorModule']['m_Gradient']['m_MaxGradient']
        fade_start = gradient['m_Atime2'] / 65535
        fade_end = gradient['m_Atime3'] / 65535
        # approximation: source fade-out duration varies with randomized lifetime; use its mean.
        fade_duration = (fade_end - fade_start) * (lifetime['m_MinScalar'] + lifetime['m_Scalar']) / 2
        values = [60., lifetime['m_MinScalar'], lifetime['m_Scalar'],
                  radius, spawn_x, spawn_y,
                  min(vx), max(vx), min(vy), max(vy),
                  size['m_MinScalar'], size['m_Scalar'], transform['m_LocalPosition']['m_X'],
                  transform['m_LocalPosition']['m_Y'], fade_duration]
        if not particle['m_Looping'] or shape['m_Type'] != 12 or emission['m_MinScalar'] != 10:
            raise ValueError('Unsupported Focus particle source')
        emitters.append((clip_name, shape['m_Type'], values))

    output.mkdir(parents=True, exist_ok=True)
    binary = bytearray(struct.pack('<4sHH', b'FOC1', 5, len(emitters)))
    for clip in clips[:5]:
        kind, source_clip = clip_identity[clip['name']]
        if kind == 3:
            position = ring_transform['m_LocalPosition']
            scale = ring_transform['m_LocalScale']
            values = [position['m_X'], position['m_Y'], scale['m_X'], scale['m_Y'], 0., 0.]
        elif kind == 4:
            # Spell Control spawns White Flash R at Self, overriding prefab position.
            scale = flash_transform['m_LocalScale']
            duration = flash_fade['fadeDuration']
            values = [0., 0., scale['m_X'], scale['m_Y'], duration, duration]
        else:
            obj = burst if kind == 2 else lines
            transform = read(obj['transform'])
            scale = transform['m_LocalScale']
            values = [transform['m_LocalPosition']['m_X'], transform['m_LocalPosition']['m_Y'],
                      scale['m_X'], scale['m_Y'], len(source_clip['frames']) / source_clip['fps'],
                      0.]
        binary.extend(struct.pack('<48sB3x6f', clip['name'].encode('ascii'), kind, *values))
    for name, shape, values in emitters:
        binary.extend(struct.pack('<48sI15f', name.encode('ascii'), shape, *values))
    (output / 'focus-effects.bin').write_bytes(binary)
    (output / 'focus-effects-manifest.json').write_text(json.dumps({
        'format': 'FOC1', 'sprites': [clip['name'] for clip in clips],
        'emitters': [{'name': name, 'shape': shape, 'values': values} for name, shape, values in emitters],
        'source_sha256': manifest['source_sha256'],
        'note': ('Emitter rate uses the FSM active value of 60/s (module default 10/s). '
                 'Cone motion is represented by independent axis bounds; particle fade uses the '
                 'source gradient fade-out interval at mean lifetime, without its fade-in.')
    }, indent=2) + '\n', encoding='utf-8')
    return sprites, clips, inputs
