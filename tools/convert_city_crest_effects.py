"""Compile City gate dust from targeted UnityPy snapshots; original art stays private."""
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess

import converted_files
from PIL import Image
from assetripper_client import ROOT
from atlas_packing import pack_pages
from convert_decor_effects import limits, particle_alpha
from convert_room_visuals import project
from convert_world_switch_visuals import bank_bytes
from private_output_paths import is_private_output
from scene_lighting import scene_ambient
from city_crest_slam_art import SOURCE as SLAM_SOURCE, compile_slam
from convert_city_crest import SOURCE as GATE_SOURCE
from inspect_fsm import decode_fsm
from tool_paths import TEX3DS as TEX3DS_EXE

SOURCE = ROOT / '_private/minimum-obstacles/city-crest-effects'
ROCKS_SOURCE = ROOT / '_private/minimum-obstacles/city-crest-rocks'
OWNERS = (4030, 4034, 4050, 4027, 4047, 4051, 4033)
RATES = (400, 400, 400, 600, 2000, 1200, 6)
DURATIONS = (.4, .5, .4, .1, .1, .1, 1)
RECORD = struct.Struct('<6I89f')


def require(ok, message):
    if not ok:
        raise ValueError(message)


def canonical(value):
    """Adapt UnityPy field casing for the existing source curve/project helpers."""
    if isinstance(value, list):
        return [canonical(v) for v in value]
    if not isinstance(value, dict):
        return value
    return {(k if k.startswith('m_') else 'm_' + k[0].upper() + k[1:]): canonical(v)
            for k, v in value.items()}


def compile_data(source, *, rocks=False, station=False, first_sprite=0):
    read = lambda n: json.loads((source / f'level{244 if station else 186}-{n}.json').read_text())
    report = json.loads((source / 'source-report.json').read_text())
    owners, rates, durations = ((1176,), (190,), (.3,)) if station else ((4031,), (20,), (1.5,)) if rocks else (OWNERS, RATES, DURATIONS)
    binary = bytearray(struct.pack('<4sI', b'CCP1', len(owners)))
    sprites, clips, records = [], [], []
    ambient = scene_ambient()
    for index, owner in enumerate(owners):
        raw = read(owner)
        p = canonical(raw)
        initial, shape = p['m_InitialModule'], p['m_ShapeModule']
        emission, uv = p['m_EmissionModule'], p['m_UVModule']
        require(not p['m_Looping'] and not p['m_Prewarm'] and p['m_MoveWithTransform'] == (0 if index == 6 else 1) and
                not emission['m_BurstCount'] and not emission['m_Bursts'] and
                math.isclose(p['m_LengthInSec'], durations[index], abs_tol=1e-6) and
                limits(emission['m_RateOverTime']) == [rates[index]] * 2 and
                shape['m_Type'] == (12 if station else 5 if index in (0, 1, 3) else 12), 'Changed City dust emission')
        require(not any(p['m_' + name]['m_Enabled'] for name in
                        ('CollisionModule', 'NoiseModule', 'SubModule', 'SizeBySpeedModule',
                         'ColorBySpeedModule')) and
                (rocks or not p['m_RotationBySpeedModule']['m_Enabled']) and
                initial['m_StartColor']['m_MinMaxState'] == 0 and not initial['m_Size3D'],
                'Unsupported City dust module')
        row = next(c for c in report['children'] if c['game_object'] == raw['m_GameObject']['m_PathID'])
        renderer = read(next(c['id'] for c in row['components'] if c['type'] == 'ParticleSystemRenderer'))
        transform = next(c['id'] for c in row['components'] if c['type'] == 'Transform') if station else row['transform']
        tr = canonical(read(transform))
        chain = [tr]
        while tr['m_Father']['m_PathID']:
            require(len(chain) < 32, 'Cyclic City dust transform')
            tr = canonical(read(tr['m_Father']['m_PathID']))
            chain.append(tr)
        follows = any(t['m_GameObject']['m_PathID'] == 1466 for t in chain)
        origin = project((0, 0, 0), chain)
        rotation = copy.deepcopy(chain)
        for tr in rotation:
            tr['m_LocalScale'] = dict(m_X=1, m_Y=1, m_Z=1)
            tr['m_LocalPosition'] = dict(m_X=0, m_Y=0, m_Z=0)
        axes, directions = [], []
        for axis in range(3):
            point = [0, 0, 0]
            point[axis] = 1
            axes.extend(a-b for a, b in zip(project(point, chain), origin))
            directions.extend(project(point, rotation))
        force, velocity, clamp = (p['m_' + k] for k in ('ForceModule', 'VelocityModule', 'ClampVelocityModule'))
        if station:
            require(not any(m['m_Enabled'] for m in (force, velocity)) and not p['m_PlayOnAwake'] and
                    clamp['m_Enabled'] and not clamp['m_SeparateAxis'] and limits(clamp['m_Drag']) == [0, 0] and
                    not uv['m_Enabled'] and not initial['m_Size3D'], 'Changed station motion modules')
        values = [p['m_LengthInSec'], limits(p['m_StartDelay'])[1]]
        for field in ('m_StartLifetime', 'm_StartSpeed', 'm_StartSize', 'm_StartRotation'):
            values.extend(limits(initial[field]))
        values += [limits(initial['m_GravityModifier'])[1], shape['m_Radius']['m_Value'],
                   math.tan(math.radians(shape['m_Angle'])), math.radians(shape['m_Arc']['m_Value']),
                   clamp['m_Dampen'] if clamp['m_Enabled'] else 0,
                   limits(clamp['m_Magnitude'])[1] if clamp['m_Enabled'] else 1e6,
                   *origin[:2], *axes, *directions]
        for module in (force, velocity):
            for axis in 'XYZ':
                values.extend(limits(module['m_' + axis]) if module['m_Enabled'] else [0, 0])
        spin = p['m_RotationBySpeedModule'] if rocks else p['m_RotationModule']
        values += limits(spin['m_Curve']) if spin['m_Enabled'] else [0, 0]
        for side in (0, 1):
            values += [limits(p['m_SizeModule']['m_Curve'], i/7)[side]
                       if p['m_SizeModule']['m_Enabled'] else 1 for i in range(8)]
        values += [particle_alpha(p['m_ColorModule'], i/7) for i in range(8)]
        values += [rates[index]] * 8
        values += [shape['m_Scale']['m_' + a] for a in 'XYZ'] + [shape['m_Length']]
        require(len(values) == 86 and all(math.isfinite(v) for v in values), 'Invalid City dust record')
        material_id, texture_id = (170, 248) if rocks else (146, 371) if index < 6 else (162, 438)
        require(renderer['m_Materials'] == [dict(m_FileID=2 if station else 4, m_PathID=material_id), dict(m_FileID=0, m_PathID=0)], 'Changed dust material')
        material = json.loads((source / f'resources-{material_id}.json').read_text())
        colors = dict(material['m_SavedProperties']['m_Colors'])
        tint, start = colors['_Color'], raw['InitialModule']['startColor']['maxColor']
        rgba = [tint[c] * start[c] * (ambient[i] if i < 3 else 1) for i, c in enumerate('rgba')]
        bitmap = Image.open(source / f'resources-{texture_id}.png').convert('RGBA')
        columns, rows = (uv['m_TilesX'], uv['m_TilesY']) if uv['m_Enabled'] else (1, 1)
        require((columns, rows) == ((1, 4) if rocks else (1, 1) if index < 6 else (7, 2)), 'Changed City dust sheet')
        if rocks:
            require(uv['m_AnimationType'] == 1 and uv['m_RowMode'] == 1 and
                    spin['m_Enabled'] and not spin['m_SeparateAxes'] and
                    spin['m_Range'] == dict(m_X=0, m_Y=1), 'Changed City rock row/rotation mode')
        first = len(sprites) + first_sprite
        for row_index in range(rows):
            for col in range(columns):
                image = bitmap.crop((col*bitmap.width//columns, row_index*bitmap.height//rows,
                                     (col+1)*bitmap.width//columns, (row_index+1)*bitmap.height//rows))
                # Preserve the original crop pixels and square Unity billboard geometry.
                image = Image.merge('RGBA', tuple(ch.point(lambda v, c=c: round(v*max(0, min(1, rgba[c]))))
                                                  for c, ch in enumerate(image.split())))
                sprites.append((len(sprites) + first_sprite, image, (-.5, -.5, .5, .5)))
        count = columns * rows
        clips.append(dict(name=f'City dust {owner}', fps=1, wrapMode=0, loopStart=0,
                          frames=[dict(spriteId=i) for i in range(first, first+count)]))
        flags = (not force['m_InWorldSpace']) | ((not velocity['m_InWorldSpace']) << 1) | (follows << 2) | ((p['m_MoveWithTransform'] == 0) << 3)
        # Finite nonloop rates bound real births; source maxParticles is deliberately much larger.
        capacity = round(durations[index] * rates[index])
        binary.extend(RECORD.pack(owner, shape['m_Type'], first, count, flags, capacity,
                                  *values, origin[2], renderer['m_SortingLayer'], renderer['m_SortingOrder']))
        records.append(dict(owner=owner, capacity=capacity, follows_gate=follows, origin=origin, values=values))
    return binary, sprites, clips, records


def rock_ranges(source):
    p = canonical(json.loads((source / 'level186-4031.json').read_text()))
    fsm = decode_fsm(json.loads((GATE_SOURCE / 'level186-5613.json').read_text())['m_Structure']['fsm'])
    actions = [a['parameters'] for s in fsm['states'] if s['name'] == 'Bump' for a in s['actions']
               if a['type'].endswith('.PlayParticleEmitter') and
               a['parameters']['gameObject']['gameObject']['name'] == 'Rocks']
    require(len(actions) == 1 and actions[0]['emit']['value'] == 0, 'Changed City rock Bump dispatch')
    return [*limits(p['m_InitialModule']['m_GravityModifier']),
            *limits(p['m_EmissionModule']['m_RateOverTime']),
            p['m_RotationBySpeedModule']['m_Range']['m_X'], p['m_RotationBySpeedModule']['m_Range']['m_Y']]


def convert(source, output, tex3ds):
    require(is_private_output(output), 'City dust output must be private')
    data, sprites, clips, emitters = compile_data(source)
    slam_sprites, slam_clip, slam_data = compile_slam(SLAM_SOURCE, source, len(sprites), canonical, require)
    sprites.extend(slam_sprites)
    clips.append(slam_clip)
    rock_data, rock_sprites, rock_clips, rock_emitters = compile_data(ROCKS_SOURCE, rocks=True, first_sprite=len(sprites))
    sprites.extend(rock_sprites)
    clips.extend(rock_clips)
    records, pages = pack_pages(sprites, [(1024, 1024)])
    output.mkdir(parents=True, exist_ok=True)
    for index, page in enumerate(pages):
        png = output / f'city-crest-fx-page-{index}.png'
        page.save(png)
        subprocess.run([str(tex3ds), '-f', 'rgba4', '-z', 'none', '-o',
                        str(output / f'city-crest-fx-page-{index}.t3x'), str(png)], check=True)
    (output / 'city-crest-fx-art.bin').write_bytes(bank_bytes(clips, list(range(len(sprites))), records, len(pages)))
    (output / 'city-crest-fx.bin').write_bytes(data)
    (output / 'city-crest-slam.bin').write_bytes(slam_data)
    (output / 'city-crest-rocks.bin').write_bytes(b'CCR1' + rock_data[4:] + struct.pack('<6f', *rock_ranges(ROCKS_SOURCE)))
    report = dict(emitters=emitters+rock_emitters, capacity=sum(e['capacity'] for e in emitters+rock_emitters), texture_bytes=len(pages)*2097152,
                  approximations=['eight-sample curves/mean alpha gradients', 'original texture crops with shared ambient lighting',
                                  'projected 3D motion and shared ambient Sprites/Lit approximation'],
                  sha256={p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in source.glob('*.json')})
    (output / 'city-crest-fx-manifest.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f"City dust/rocks: {len(emitters)+len(rock_emitters)} emitters, {len(sprites)} sprites, {report['capacity']} bounded particles")


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=SOURCE)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--tex3ds', type=Path, default=Path(TEX3DS_EXE))
    args = parser.parse_args()
    convert(args.source, args.output, args.tex3ds)
