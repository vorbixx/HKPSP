"""Compile original camera-follow motes, leaves and Box drips for scene zones.
"""
import hashlib
import json
from pathlib import Path
import struct

from PIL import Image
from convert_ambient_particles import ROOT, _pack_record, _range, mesh_edges

SOURCE = (ROOT / '_private/research').resolve().parent / 'fungus-particles'


def particle_source(visual_source):
    manager = visual_source.parent / 'scene-manager.json'
    if not manager.is_file():
        return None
    settings = json.loads(manager.read_text())
    settings = settings.get('m_Structure', settings)
    zone = settings.get('overrideParticlesWith') or settings.get('mapZone')
    if settings.get('noParticles') or zone not in (6, 7, 8, 9):
        return None
    return SOURCE / f'zone{zone}'


def load(source):
    report = json.loads((source / 'ambient-particles.json').read_text())
    for name, expected in report['source_sha256'].items():
        if hashlib.sha256((source / name).read_bytes()).hexdigest() != expected:
            raise ValueError(f'Changed scene particle source: {name}')
    emitters = []
    images = {}
    for original in report['derived']['emitters']:
        row = dict(original)
        row['sprite_count'] = 1
        row['spin'] = (0.0, 0.0)
        row['half_height'] = 0.0
        system = json.loads((source / row['system']).read_text())
        shader = json.loads((source / row['shader']).read_text())['m_Name']
        shape = system['m_ShapeModule']
        row['box'] = not row['mesh'] and shape['m_Type'] == 5
        rotation = system['m_RotationModule']
        if not row['mesh'] and (shape['m_Type'], shader) not in (
                (12, 'Sprites/Default'), (5, 'Sprites/Lit')):
            continue
        if rotation['m_Enabled']:
            if rotation['m_SeparateAxes'] or rotation['m_Curve']['m_MinMaxState'] not in (0, 3):
                raise ValueError('Unsupported scene particle rotation')
            row['spin'] = sorted(_range(rotation['m_Curve']))
        if not row['mesh'] and shape['m_Type'] == 5:
            # Shape 5 is Box volume; BoxShell/BoxEdge are distinct source types.
            # Only the original billboard drips are supported here.
            renderer = json.loads((source / row['renderer']).read_text())
            if (renderer['m_RenderMode'] != 0 or shape['m_PlacementMode'] != 0 or
                    _range(system['m_InitialModule']['m_StartSpeed']) != (0.0, 0.0)):
                raise ValueError('Unsupported particle Box renderer/speed')
            if any(shape[name][f'm_{axis}'] != 0 for name in ('m_Position', 'm_Rotation') for axis in 'XYZ'):
                raise ValueError('Unsupported transformed particle Box')
            transform = json.loads((source / row['transforms'][0]).read_text())
            row['half_height'] = abs(transform['m_LocalScale']['m_Y'] * shape['m_Scale']['m_Y']) * .5
        if not row['mesh'] and shape['m_Type'] == 12:
            uv = system['m_UVModule']
            if (not uv['m_Enabled'] or
                    (uv['m_TilesX'], uv['m_TilesY'], uv['m_AnimationType'], uv['m_RowMode']) != (1, 3, 1, 1) or
                    rotation['m_SeparateAxes'] or rotation['m_Curve']['m_MinMaxState'] not in (0, 3)):
                raise ValueError('Unsupported sprite leaf animation')
            # approximation: one of the three authored sheet rows per birth.
            row['sprite_count'] = 3
            if rotation['m_Enabled']:
                row['spin'] = sorted(_range(rotation['m_Curve']))
        image = Image.open(source / report['media'][row['texture']]['png']).convert('RGBA')
        count = row['sprite_count']
        if image.height % count:
            raise ValueError('Particle sheet rows must have equal height')
        row['image_keys'] = ([row['texture']] if count == 1 else
                             [f"{row['texture']}#row{i}" for i in range(count)])
        for i,key in enumerate(row['image_keys']):
            images[key] = image.crop((0, i*image.height//count, image.width, (i+1)*image.height//count))
        emitters.append(row)
    if not 0 < len(emitters) <= 8 or not images:
        raise ValueError('Unsupported scene particle bank')
    images = dict(sorted(images.items()))
    return report, emitters, images


def compile_config(source, sprite_index, ambient=(1.0, 1.0, 1.0)):
    report, emitters, images = load(source)
    sprites = {texture: sprite_index + i for i, texture in enumerate(images)}
    records = []
    version = (5 if any(row['box'] for row in emitters) else
               4 if any(row['sprite_count'] > 1 or any(row['spin']) for row in emitters) else 3)
    total = 0
    for row in emitters:
        if len(row['transforms']) != 2:
            raise ValueError('Unsupported scene particle transform hierarchy')
        def resource_id(key):
            if not key.startswith('resources.assets-'):
                raise ValueError(f'Unexpected scene particle collection: {key}')
            return key.removeprefix('resources.assets-').removesuffix('.json')
        shader = json.loads((source / row['shader']).read_text())['m_Name']
        if shader not in ('Legacy Shaders/Particles/Additive', 'Legacy Shaders/Particles/Additive (Soft)', 'Sprites/Default', 'Sprites/Lit'):
            raise ValueError(f'Unsupported scene particle shader: {shader}')
        # approximation: lit drips receive the shared diffuse ambient, without per-pixel lights.
        record = _pack_record((row['name'], resource_id(row['system']),
                               resource_id(row['transforms'][0]), resource_id(row['renderer'])),
                              sprite_index, source=source,
                              material_id=resource_id(row['material']),
                              parent_id=resource_id(row['transforms'][1]),
                              soft_particle=shader == 'Legacy Shaders/Particles/Additive (Soft)',
                              sprite_particle=shader in ('Sprites/Default', 'Sprites/Lit'),
                              ambient=ambient if shader == 'Sprites/Lit' else (1.0, 1.0, 1.0))
        total += struct.unpack_from('<I', record)[0]
        extension = (struct.pack('<2fI', *row['spin'], row['sprite_count']) if version >= 4 else b'')
        if version >= 5:
            extension += struct.pack('<f', row['half_height'])
        records.append(struct.pack('<I', sprites[row['image_keys'][0]]) + record + extension)
    if not 0 < total <= 512:
        raise ValueError('Scene particle capacity exceeds fixed runtime pool')
    edges = mesh_edges()
    return (struct.pack('<4s5I', b'HAP1', version, len(records), total, sprite_index, len(edges))
            + b''.join(records) + b''.join(struct.pack('<5f', *edge) for edge in edges))
