"""Compile the verified Tutorial_01 ambient particle export into HAP1 v2."""
import converted_files  # Install hardlink copy-on-write before asset writes.
import hashlib
import json
import math
from pathlib import Path
import struct

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / '_private/research/ambient-particles'
MANIFEST = SOURCE / 'ambient-particles.json'
EMITTERS = (
    ('immediate_BG', '14663', '10881', '15019'),
    ('FG', '14599', '10945', '14870'),
    ('far BG', '14621', '9936', '14810'),
    ('FG (1)', '14610', '11036', '14906'),
)
SAMPLES = 16
MESH = SOURCE / 'mesh-10209.json'
MESH_SOURCE_SHA256 = 'd656c46a47e39c4346e1ae506f6cd8328408ff2d97156274d8ac7adf7b844513'
MESH_GEOMETRY_SHA256 = 'd57d3cd878b67afdf3a103ef28b996625a3924b69f465733e045f19fbacfe667'
ROW = struct.Struct('<IiiI135f')


def mesh_edges(mesh=None):
    if mesh is None:
        mesh = json.loads(MESH.read_text())
    geometry = json.dumps([mesh['vertices'], mesh['triangles']], separators=(',', ':')).encode()
    if (mesh['pathId'] != 10209 or mesh['sourceSha256'] != MESH_SOURCE_SHA256
            or hashlib.sha256(geometry).hexdigest() != MESH_GEOMETRY_SHA256):
        raise ValueError('Changed ambient Plane10209 geometry')
    vertices = mesh['vertices']
    edges = sorted({tuple(sorted((a, b))) for triangle in mesh['triangles']
                    for a, b in zip(triangle, triangle[1:] + triangle[:1])})
    if len(edges) != 320:
        raise ValueError('Unexpected ambient mesh edge count')
    # approximation: unique triangle edges, local-length weighting and uniform
    # interpolation; native CacheEdges stores lengths, but its consumer is unresolved.
    cumulative = 0.0
    result = []
    for a, b in edges:
        start, end = vertices[a], vertices[b]
        cumulative += math.dist(start, end)
        result.append((start[0], start[2], end[0], end[2], cumulative))
    return result


def _resource(path_id):
    return json.loads((SOURCE / f'resources.assets-{path_id}.json').read_text())


def _curve(curve, t):
    keys = curve['m_Curve']
    if not keys:
        return 0.0
    if t <= keys[0]['m_Time']:
        return float(keys[0]['m_Value'])
    for a, b in zip(keys, keys[1:]):
        if t <= b['m_Time']:
            span = b['m_Time'] - a['m_Time']
            if span <= 0:
                return float(b['m_Value'])
            u = (t - a['m_Time']) / span
            u2, u3 = u*u, u*u*u
            return ((2*u3-3*u2+1)*a['m_Value'] + (u3-2*u2+u)*span*a['m_OutSlope']
                    + (-2*u3+3*u2)*b['m_Value'] + (u3-u2)*span*b['m_InSlope'])
    return float(keys[-1]['m_Value'])


def _minmax_samples(value):
    state = value['m_MinMaxState']
    if state == 0:
        return [float(value['m_Scalar'])] * SAMPLES
    if state == 1:
        curve = value['m_MaxCurve']
        if not curve['m_Curve']:
            return [float(value['m_Scalar'])] * SAMPLES
        return [_curve(curve, i/(SAMPLES-1))*value['m_Scalar'] for i in range(SAMPLES)]
    if state == 2:
        curve = value['m_MaxCurve']
        if not curve['m_Curve']:
            return [float(value['m_Scalar'])] * SAMPLES
        return [_curve(curve, i/(SAMPLES-1))*value['m_Scalar'] for i in range(SAMPLES)]
    if state == 3:
        midpoint = (float(value['m_MinScalar']) + float(value['m_Scalar'])) * .5
        return [midpoint] * SAMPLES
    raise ValueError(f'Unknown MinMaxCurve state {state}')


def _range(value):
    """Return authored low/high constants for a runtime per-particle draw."""
    state = value['m_MinMaxState']
    if state == 0:
        return float(value['m_Scalar']), float(value['m_Scalar'])
    if state == 3:
        return float(value['m_MinScalar']), float(value['m_Scalar'])
    if state == 1:
        samples = [_curve(value['m_MaxCurve'], i/(SAMPLES-1))*value['m_Scalar']
                   for i in range(SAMPLES)]
        mean = sum(samples)/SAMPLES
        return mean, mean
    if state == 2:
        samples = [(_curve(value['m_MinCurve'], i/(SAMPLES-1))*value['m_MinScalar']
                    + _curve(value['m_MaxCurve'], i/(SAMPLES-1))*value['m_Scalar'])*.5
                   for i in range(SAMPLES)]
        mean = sum(samples)/SAMPLES
        return mean, mean
    raise ValueError(f'Unknown MinMaxCurve state {state}')


def _gradient_samples(gradient):
    colors = []
    alphas = []
    for i in range(gradient['m_NumColorKeys']):
        c = gradient[f'm_Key{i}']
        colors.append((gradient[f'm_Ctime{i}']/65535.0,
                       tuple(float(c[f'm_{channel}']) for channel in 'RGBA')))
    for i in range(gradient['m_NumAlphaKeys']):
        alphas.append((gradient[f'm_Atime{i}']/65535.0,
                       float(gradient[f'm_Key{i}']['m_A'])))
    def sample(keys, t):
        if not keys:
            return 1.0
        if t <= keys[0][0]: return keys[0][1]
        for (ta, va), (tb, vb) in zip(keys, keys[1:]):
            if t <= tb:
                u = (t-ta)/(tb-ta) if tb != ta else 1.0
                if isinstance(va, tuple): return tuple(a+(b-a)*u for a,b in zip(va,vb))
                return va+(vb-va)*u
        return keys[-1][1]
    return [(*sample(colors, i/(SAMPLES-1))[:3], sample(alphas, i/(SAMPLES-1)))
            for i in range(SAMPLES)]


def _vec(value, prefix):
    return [float(value[f'{prefix}_{axis}']) for axis in 'XYZ']


def _rgba(value):
    return [float(value[f'm_{c}']) for c in 'RGBA']


def _pack_record(entry, sprite_index, density=1.0, *, source=None, resource_override=None,
                 material_id='135', parent_id='10295', soft_particle=False,
                 sprite_particle=False, ambient=(1.0, 1.0, 1.0)):
    if len(ambient) != 3 or not all(math.isfinite(v) and v >= 0 for v in ambient):
        raise ValueError('Invalid particle ambient light')
    # Share the Tutorial curve/plane contract with other camera particle banks.
    def resource(path_id):
        if resource_override is not None:
            return resource_override(path_id)
        if source is None:
            return _resource(path_id)
        return json.loads((source / f'resources.assets-{path_id}.json').read_text())
    name, system_id, transform_id, renderer_id = entry
    system = resource(system_id)
    transform = resource(transform_id)
    renderer = resource(renderer_id)
    if not system['m_InitialModule']['m_Enabled'] or not system['m_EmissionModule']['m_Enabled']:
        raise ValueError(f'{name}: disabled particle source')
    init = system['m_InitialModule']
    shape = system['m_ShapeModule']
    emission = system['m_EmissionModule']
    color_module = system['m_ColorModule']
    size_module = system['m_SizeModule']
    velocity = system['m_VelocityModule']
    capacity = int(init['m_MaxNumParticles'])
    pos = _vec(transform['m_LocalPosition'], 'm')
    rotation = [float(transform['m_LocalRotation'][f'm_{axis}']) for axis in 'XYZW']
    scale = _vec(transform['m_LocalScale'], 'm')
    if sprite_particle:
        if shape['m_Type'] not in (5, 12) or init['m_Size3D'] or init['m_Rotation3D']:
            raise ValueError(f'{name}: unsupported sprite particle shape')
        if shape['m_Type'] == 5:
            half_extent = [abs(shape['m_Scale'][f'm_{a}'] * scale[i]) * .5 for a,i in (('X',0),('Z',2))]
        else:
            # approximation: distribute the authored shape-12 leaves along local X.
            half_extent = [float(shape['m_RadiusParameter']['m_Value'])*abs(scale[0]), 0.0]
    else:
        if (shape['m_Type'], shape['m_PlacementMode'], shape['m_MeshSpawn']['m_Mode'],
                shape['m_Mesh']['m_PathID']) != (6, 1, 0, 10209):
            raise ValueError(f'{name}: expected Mesh/Edge/Random Plane10209')
        # The built-in Plane is in XZ; its stored cone angle is inactive for Mesh.
        half_extent = [5.0*abs(scale[0]), 5.0*abs(scale[2])]
    shape_angle = 0.0
    rate = emission['m_RateOverTime']
    lifetime = init['m_StartLifetime']
    start_size = init['m_StartSize']
    start_speed = init['m_StartSpeed']
    gravity = init['m_GravityModifier']
    start_color = init['m_StartColor']
    color_min, color_max = _rgba(start_color['m_MinColor']), _rgba(start_color['m_MaxColor'])
    if start_color['m_MinMaxState'] == 0:
        color_min = color_max
    color_gradient = color_module['m_Gradient']
    color_samples = _gradient_samples(color_gradient['m_MaxGradient'])
    material = resource(material_id)
    # Additive (Soft) has no _TintColor property; ignore stale serialized values.
    colors = material['m_SavedProperties']['m_Colors']
    tint_rgba = ([a*b for a,b in zip(_rgba(colors['_Color']), _rgba(colors['_RendererColor']))]
                 if sprite_particle else [1.0]*4 if soft_particle else _rgba(colors['_TintColor']))
    if sprite_particle:
        # Sprite material/light applies even without Color over Lifetime.
        tint_rgba = [t * light for t,light in zip(tint_rgba, (*ambient, 1.0))]
        color_min = [c*t for c,t in zip(color_min,tint_rgba)]
        color_max = [c*t for c,t in zip(color_max,tint_rgba)]
    else:
        color_samples = [tuple(c*t for c,t in zip(sample,tint_rgba)) for sample in color_samples]
    size_curve = size_module['m_Curve']
    if size_module['m_Enabled'] and size_curve['m_MinMaxState'] == 3:
        size_samples = [float(size_curve['m_MinScalar'])]*SAMPLES
        size_max_samples = [float(size_curve['m_Scalar'])]*SAMPLES
    else:
        size_samples = _minmax_samples(size_curve) if size_module['m_Enabled'] else [1.0]*SAMPLES
        size_max_samples = size_samples
    velocity_ranges = []
    for axis in 'XYZ':
        item = velocity[f'm_{axis}']
        velocity_ranges.extend(sorted(_range(item)) if velocity['m_Enabled'] else [0.0, 0.0])
    flags = ((1 if color_module['m_Enabled'] else 0) | (2 if size_module['m_Enabled'] else 0)
             | (4 if start_color['m_MinMaxState'] == 2 else 0)
             | (8 if soft_particle else 0) | ((16 | (128 if shape['m_Type'] == 5 else 32)) if sprite_particle else 0)
             | (64 if velocity['m_Enabled'] and velocity['m_InWorldSpace'] else 0))
    # Store Unity's sorting-layer ordinal, not its opaque hash ID.  The
    # runtime compares this value with RoomDraw.values[11]; foreground
    # particle systems use layer 11 while background systems use layer 0.
    sorting_layer = int(renderer['m_SortingLayer'])
    if sorting_layer < 0 or sorting_layer > 255:
        raise ValueError(f'{name}: invalid sorting layer {sorting_layer}')
    ints = (capacity, sorting_layer, int(renderer['m_SortingOrder']), flags)
    parent = resource(parent_id)
    parent_pos = _vec(parent['m_LocalPosition'], 'm')
    pos = [a+b for a,b in zip(parent_pos,pos)]
    floats = (pos + rotation + scale + half_extent + [shape_angle]
              + [v*density for v in _range(rate)] + list(_range(lifetime)) + list(_range(start_size))
              + list(_range(start_speed)) + list(_range(gravity))
              + velocity_ranges + color_min + color_max
              + [v for color in color_samples for v in color] + size_samples
              + list(_range(init['m_StartRotation'])) + size_max_samples)
    return ROW.pack(*ints, *floats)


def compile_config(sprite_index, density=1.0):
    if not math.isfinite(density) or not 0 < density <= 1:
        raise ValueError('Ambient density must be in (0, 1]')
    manifest = json.loads(MANIFEST.read_text())
    for relative, expected in manifest['source_sha256'].items():
        if hashlib.sha256((SOURCE / relative).read_bytes()).hexdigest() != expected:
            raise ValueError(f'Changed ambient dependency: {relative}')
    if manifest['derived']['total_capacity'] != 184:
        raise ValueError('Unexpected ambient particle capacity')
    edges = mesh_edges()
    header = struct.pack('<4s5I', b'HAP1', 2, len(EMITTERS), 184, int(sprite_index), len(edges))
    return (header + b''.join(_pack_record(e, sprite_index, density) for e in EMITTERS)
            + b''.join(struct.pack('<5f', *edge) for edge in edges))



def compile_fungus_config(source, sprite_index):
    """Map-zone9 emitters with source415/406 in adjacent resident atlas slots."""
    manifest = json.loads((source / 'room-visuals-map.json').read_text())
    raw = (source / 'fungus-ambient.json').read_bytes()
    if hashlib.sha256(raw).hexdigest() != manifest['source_sha256']['fungus-ambient.json']:
        raise ValueError('Changed Fungus ambient source')
    rows = json.loads(raw)['emitters']
    if [r['owner'] for r in rows] != [4165, 5316, 4229, 6084]:
        raise ValueError('Changed Fungus particle hierarchy')
    records = []
    capacity = 0
    for row in rows:
        texture = dict(row['material']['m_SavedProperties']['m_TexEnvs'])['_MainTex']['m_Texture']
        if texture['m_PathID'] not in (415, 406):
            raise ValueError('Unsupported Fungus ambient texture')
        chain = row['chain']
        parent = {'m_LocalPosition': dict(m_X=0, m_Y=0, m_Z=0)}
        for transform in chain[1:]:
            if (list(transform['m_LocalRotation'].values()) != [0, 0, 0, 1]
                    or list(transform['m_LocalScale'].values()) != [1, 1, 1]):
                raise ValueError('Unsupported ambient parent transform')
            for axis in 'XYZ':
                parent['m_LocalPosition']['m_'+axis] += transform['m_LocalPosition']['m_'+axis]
        resources = dict(system=row['system'], transform=chain[0], renderer=row['renderer'],
                         material=row['material'], parent=parent)
        records.append(struct.pack('<I', sprite_index + (texture['m_PathID'] == 406)) + _pack_record((row['name'], 'system', 'transform', 'renderer'),
            sprite_index, resource_override=resources.__getitem__, material_id='material', parent_id='parent'))
        capacity += row['system']['m_InitialModule']['m_MaxNumParticles']
    if capacity != 441:
        raise ValueError('Changed Fungus source capacity')
    edges = mesh_edges()
    return (struct.pack('<4s5I', b'HAP1', 3, len(rows), capacity, sprite_index, len(edges))
            + b''.join(records) + b''.join(struct.pack('<5f', *edge) for edge in edges))

def artwork(soft=False):
    source = SOURCE.with_name('ambient-particles-fungal') if soft else SOURCE
    manifest = json.loads((source / 'ambient-particles.json').read_text())
    image_name = manifest['media'][f'resources.assets-{406 if soft else 415}.json']['png']
    raw = (source / image_name).read_bytes()
    if hashlib.sha256(raw).hexdigest() != manifest['source_sha256'][image_name]:
        raise ValueError('Changed ambient sprite')
    return Image.open(source / image_name).convert('RGBA')


if __name__ == '__main__':
    print(json.dumps({'png_size': artwork().size, 'config_bytes': len(compile_config(0))}))
