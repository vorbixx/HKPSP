"""Compile saved Deepnest controller overrides into the shared ambient atlas/HAP1 v3.

Approximation: preserve the existing mesh-edge sampling and lifetime curve sampling;
Sprites/Lit rubble uses its material tint without dynamic directional lighting.
"""
import hashlib
import json
import math
from pathlib import Path
import struct
from PIL import Image
from convert_ambient_particles import (ROW, _range, _rgba, _vec, _gradient_samples,
                                       _minmax_samples, mesh_edges, SAMPLES)

SOURCE = Path(__file__).resolve().parents[1] / '_private/research/deepnest-ambient-particles'
MANIFEST = SOURCE / 'ambient-particles.json'
EXTRA = struct.Struct('<3I13f')  # shape, sprite, material; half-size XYZ, basis, alpha gain


def read(name):
    return json.loads((SOURCE / name).read_text())


def manifest():
    data = read('ambient-particles.json')
    for name, expected in data['source_sha256'].items():
        if hashlib.sha256((SOURCE/name).read_bytes()).hexdigest() != expected:
            raise ValueError(f'Changed Deepnest ambient dependency: {name}')
    return data


def multiply(a, b):
    return [[sum(a[r][k]*b[k][c] for k in range(4)) for c in range(4)] for r in range(4)]


def transform(chain):
    matrix = [[float(r == c) for c in range(4)] for r in range(4)]
    quaternion = [0., 0., 0., 1.]
    for name in chain:  # saved root to emitter, including the rubble subgroup
        t = read(name)
        x,y,z,w = [t['m_LocalRotation'][f'm_{c}'] for c in 'XYZW']
        scale = _vec(t['m_LocalScale'], 'm')
        local = [[1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w), 0],
                 [2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w), 0],
                 [2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y), 0], [0,0,0,1]]
        for r in range(3):
            for c in range(3): local[r][c] *= scale[c]
            local[r][3] = t['m_LocalPosition'][f'm_{"XYZ"[r]}']
        matrix = multiply(matrix, local)
        a,b,c,d = quaternion
        quaternion = [d*x+a*w+b*z-c*y, d*y-a*z+b*w+c*x,
                      d*z+a*y-b*x+c*w, d*w-a*x-b*y-c*z]
    return matrix, quaternion


def sheet_rows(system):
    """Single-column random rows select one fixed sprite for each particle."""
    uv = system['m_UVModule']
    if not uv['m_Enabled']:
        return 1
    if (uv['m_Mode'], uv['m_AnimationType'], uv['m_TilesX'], uv['m_RowMode'],
            uv['m_FlipU'], uv['m_FlipV']) != (0, 1, 1, 1, 0, 0):
        raise ValueError('Unsupported Deepnest particle texture sheet')
    rows = uv['m_TilesY']
    if not isinstance(rows, int) or not 1 <= rows <= 256:
        raise ValueError('Invalid Deepnest particle sheet row count')
    return rows


def pack_emitter(entry, sprite):
    system, renderer = read(entry['system']), read(entry['renderer'])
    init, shape = system['m_InitialModule'], system['m_ShapeModule']
    emission, velocity = system['m_EmissionModule'], system['m_VelocityModule']
    color, size = system['m_ColorModule'], system['m_SizeModule']
    if not (init['m_Enabled'] and emission['m_Enabled'] and shape['m_Enabled']):
        raise ValueError('Disabled Deepnest ambient source')
    if system['m_MoveWithTransform'] != 1:
        raise ValueError('Expected world-space Deepnest particles')
    kind = shape['m_Type']
    if kind == 6:
        # The entrance and edge banks use both Unity placement modes for the
        # same Plane10209 mesh.  The saved mesh is the invariant; placement
        # mode only changes the native randomisation mode, which is an
        # approximation in the fixed edge sampler below.
        if (shape['m_MeshSpawn']['m_Mode'], shape['m_Mesh']['m_PathID']) != (0,10209):
            raise ValueError('Expected Plane10209 mesh')
        half = [5., 0., 5.]
    elif kind == 12:
        half = [float(shape['m_RadiusParameter']['m_Value']), 0., 0.]
    elif kind == 5:
        half = [.5, .5, .5]
    else:
        raise ValueError(f'Unsupported ambient shape: {kind}')
    if any(shape['m_Position'].values()) or any(shape['m_Rotation'].values()):
        raise ValueError('Unexpected ambient shape offset/rotation')
    matrix, rotation = transform(entry['transforms'])
    shape_scale = _vec(shape['m_Scale'], 'm')
    basis = [matrix[r][c]*shape_scale[c] for r in range(3) for c in range(3)]
    material = read(entry['material'])['m_SavedProperties']['m_Colors']
    shader = read(entry['shader'])['m_ParsedForm']['m_Name']
    if shader == 'Legacy Shaders/Particles/Additive':
        blend, gain, tint = 4, 2., _rgba(material['_TintColor'])
    elif shader == 'Legacy Shaders/Particles/Additive (Soft)':
        # Saved DXBC passes vertex colour straight through, then premultiplies
        # texture * vertex alpha. _TintColor is a stale, unused material value.
        blend, gain, tint = 1, 1., [1.]*4
    elif shader == 'Sprites/Lit':
        blend, gain, tint = 0, 1., [1.]*4
        for name in ('_Color', '_RendererColor'):
            tint = [a*b for a,b in zip(tint, _rgba(material.get(name, dict(zip(('m_R','m_G','m_B','m_A'),[1.]*4)))))]
    else:
        raise ValueError(f'Unsupported ambient shader: {shader}')
    start_color = init['m_StartColor']
    lo, hi = _rgba(start_color['m_MinColor']), _rgba(start_color['m_MaxColor'])
    if start_color['m_MinMaxState'] == 0: lo = hi
    gradient = (_gradient_samples(color['m_Gradient']['m_MaxGradient']) if color['m_Enabled'] else [[1.]*4]*SAMPLES)
    gradient = [[a*b for a,b in zip(sample,tint)] for sample in gradient]
    curve = size['m_Curve']
    if size['m_Enabled'] and curve['m_MinMaxState'] == 3:
        size_lo, size_hi = [curve['m_MinScalar']]*SAMPLES, [curve['m_Scalar']]*SAMPLES
    else:
        size_lo = _minmax_samples(curve) if size['m_Enabled'] else [1.]*SAMPLES
        size_hi = size_lo
    ranges = []
    for axis in 'XYZ':
        ranges.extend(_range(velocity[f'm_{axis}']) if velocity['m_Enabled'] else (0.,0.))
    # HAP1 v3 flags 8..15 hold random-row count minus one; zero keeps old banks.
    flags = (1 | (2 if size['m_Enabled'] else 0) | (4 if velocity['m_InWorldSpace'] else 0)
             | ((sheet_rows(system)-1) << 8))
    floats = ([matrix[r][3] for r in range(3)] + rotation + [1.,1.,1.]
              + [half[0],half[2],0.] + list(_range(emission['m_RateOverTime']))
              # Unity accepts reversed random endpoints (the Edge bank uses
              # 5/2.5 and 6/2.5). HAP1 requires ascending lifetime bounds.
              + sorted(_range(init['m_StartLifetime'])) + list(_range(init['m_StartSize']))
              + list(_range(init['m_StartSpeed'])) + list(_range(init['m_GravityModifier']))
              + ranges + lo + hi + [v for sample in gradient for v in sample]
              + size_lo + list(_range(init['m_StartRotation'])) + size_hi)
    row = ROW.pack(entry['capacity'], int(renderer['m_SortingLayer']), int(renderer['m_SortingOrder']), flags, *floats)
    return EXTRA.pack(kind, sprite, blend, *half, *basis, gain) + row


def append_banks(images, geometry):
    data = manifest()
    sprites = {}
    rows_by_texture = {}
    for bank in data['banks']:
        for entry in bank['emitters']:
            key = entry['texture']
            rows = sheet_rows(read(entry['system']))
            if key in rows_by_texture and rows_by_texture[key] != rows:
                raise ValueError('Inconsistent Deepnest particle texture sheets')
            rows_by_texture[key] = rows
    for texture, media in sorted(data['media'].items()):
        sprites[texture] = len(images)
        image = Image.open(SOURCE/media['png']).convert('RGBA')
        rows = rows_by_texture[texture]
        if image.height % rows:
            raise ValueError('Deepnest particle texture height is not divisible by rows')
        height = image.height // rows
        for row in range(rows):
            images.append(image.crop((0, row*height, image.width, (row+1)*height)))
            geometry.append((-.5,-.5,.5,.5))
    edges = mesh_edges()
    banks = {}
    for bank in data['banks']:
        emitters = bank['emitters']
        total = sum(e['capacity'] for e in emitters)
        if len(emitters)>5 or total>441:
            raise ValueError('Deepnest ambient bank exceeds the reserved runtime pool')
        banks[bank['zone']] = (struct.pack('<4s5I', b'HAP1', 3, len(emitters), total, 0, len(edges))
            + b''.join(pack_emitter(e,sprites[e['texture']]) for e in emitters)
            + b''.join(struct.pack('<5f', *edge) for edge in edges))
    return banks
