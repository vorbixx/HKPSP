"""Compile the five uncovered conditional emitters using the hero FX layout."""
import copy
import json
import math
import struct

from PIL import Image
from convert_decor_effects import limits, particle_alpha, scene_ambient
from convert_room_visuals import project


OWNERS = (2120, 2124, 3693, 3769, 3788)


def compile_particles(source, mapping, sprites, clips):
    read = lambda name: json.loads((source / name).read_text())
    binary = bytearray(struct.pack('<4sI', b'HKX1', len(OWNERS)))
    records = []
    for owner in OWNERS:
        key = f'level6-{owner}.json'
        entry = mapping['particles'][key]
        p = read(entry['system'])
        initial, shape = p['m_InitialModule'], p['m_ShapeModule']
        transforms = []
        tr = read(mapping['actors'][key]['Transform'][0])
        # The emitter origin is supplied by the event, relative to its root.
        # Keep every intermediate transform; the root translation is supplied below.
        root = 5987 if owner < 3000 else 7531
        while True:
            transforms.append(tr)
            parent = tr['m_Father']['m_PathID']
            if not parent or parent == root:
                break
            tr = read(f'level6-{parent}.json')
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
        values += [shape['m_Scale']['m_'+axis] for axis in 'XYZ'] + [shape['m_Length']]
        if len(values) != 86 or shape['m_Type'] not in (10, 12) or p['m_EmissionModule']['m_Bursts']:
            raise ValueError('Unsupported conditional particle source')
        material = read(entry['material'])['m_SavedProperties']
        # Dust uses Sprites/Lit, orbs Sprites/Default, impact/lava Additive Soft.
        # Random start colours are represented by their mean.
        colors = material['m_Colors']
        tint = colors['_TintColor' if owner in (2120, 3693) else '_Color']
        start = initial['m_StartColor']
        rgba = []
        for channel in 'RGBA':
            hi, lo = start['m_MaxColor']['m_'+channel], start['m_MinColor']['m_'+channel]
            rgba.append(tint['m_'+channel] * ((lo+hi)/2 if start['m_MinMaxState'] == 2 else hi))
        if owner in (2124, 3769):
            # Sprites/Lit falls back to Sprites/Diffuse in the source build.
            ambient = scene_ambient()
            rgba = [v * colors['_RendererColor']['m_'+c] * (ambient[i] if i < 3 else 1)
                    for i, (v, c) in enumerate(zip(rgba, 'RGBA'))]
        bitmap = Image.open(source / entry['texture']).convert('RGBA')
        uv = p['m_UVModule']
        columns, rows = (uv['m_TilesX'], uv['m_TilesY']) if uv['m_Enabled'] else (1, 1)
        frames = []
        for row in range(rows):
            for col in range(columns):
                image = bitmap.crop((col*bitmap.width//columns, row*bitmap.height//rows,
                                     (col+1)*bitmap.width//columns, (row+1)*bitmap.height//rows))
                # Unity maps each sheet cell onto a square particle billboard.
                # At 16 screen pixels/unit, these .3..2-unit particles occupy
                # at most 32 pixels. Keep them in the existing two atlas pages.
                image = image.resize((32, 32), Image.Resampling.LANCZOS)
                if owner in (2120, 3693):
                    # Approximation: Legacy Particles/Additive (Soft) uses
                    # 2*vertex*tint*texture and SrcAlpha/One. Encode its light
                    # contribution as straight alpha for the shared renderer;
                    # black sheet borders become transparent. Background gain
                    # and soft depth intersection remain approximate.
                    pixels = []
                    for pixel in image.getdata():
                        alpha = min(1, pixel[3]/255*rgba[3]*2)
                        light = [min(1, pixel[i]/255*rgba[i]*2)*alpha for i in range(3)]
                        strength = max(light)
                        pixels.append(tuple(round(v/strength*255) if strength else 0 for v in light)
                                      + (round(strength*255),))
                    image.putdata(pixels)
                else:
                    image = Image.merge('RGBA', tuple(ch.point(lambda v, c=c: round(v*max(0, min(1, rgba[c]))))
                                                      for c, ch in enumerate(image.split())))
                sprite = f'source-particle/{owner}/{row*columns+col}'
                sprites[sprite] = (image, (-.5, -.5, .5, .5))
                frames.append(sprite)
        clips.append(dict(name=f'event source particle {owner}', fps=1, wrap=0, loop=0, frames=frames))
        flags = (0 if force['m_InWorldSpace'] else 1) | (0 if velocity['m_InWorldSpace'] else 2)
        cap = initial['m_MaxNumParticles']
        binary.extend(struct.pack('<5I86f', owner, shape['m_Type'], columns*rows, flags, cap, *values))
        records.append(dict(owner=f'level6:{owner}', shape=shape['m_Type'], cap=cap, looping=p['m_Looping'], values=values))
    return binary, records
