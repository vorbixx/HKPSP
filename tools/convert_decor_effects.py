"""Compile the verified decor export into a bounded, approximate visual model.

Original resources remain private. This converter does not query Unity/REA.
"""
import converted_files  # Install hardlink copy-on-write before asset writes.
import hashlib
import copy
import json
import math
from pathlib import Path
import shutil
import struct
import tex3ds_cache

from PIL import Image
from atlas_packing import pack_pages
from convert_hero import decode_sprite
from convert_room_visuals import project
from convert_ambient_particles import artwork as ambient_artwork, compile_config as compile_ambient_config
from convert_deepnest_ambient import append_banks as append_deepnest_banks
from contact_effect_artwork import ContactSource, compile_bank
from breakable_owner_effects import OwnerSource, compile_owners
from inspect_fsm import decode_fsm
from scene_lighting import SCENE_LIGHT, SCENE_LIGHT_SHA256, scene_ambient
from tool_paths import TEX3DS as TEX3DS_EXE

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / '_private/research/decor-effects'
PLANT_SOURCE = ROOT / '_private/research/plant-effects'
# Combined source pool bound 7697, rounded to 128 slots.
POOL = 7808
FLASH_TEXELS_PER_UNIT = 20
GRASS_SOURCE = ROOT / '_private/research/grass-effects-source'


def trim_transparent(bitmap, bounds):
    """Remove empty margins while retaining geometry and the filter footprint."""
    box = bitmap.getbbox()
    if box is None:
        return bitmap, bounds
    x0, y0, x1, y1 = (max(0, box[0]-1), max(0, box[1]-1),
                       min(bitmap.width, box[2]+1), min(bitmap.height, box[3]+1))
    left, bottom, right, top = bounds
    dx, dy = (right-left)/bitmap.width, (top-bottom)/bitmap.height
    return bitmap.crop((x0, y0, x1, y1)), (left+x0*dx, top-y1*dy, left+x1*dx, top-y0*dy)


def prefilter_flash(bitmap, bounds):
    # approximation: integrate the source flash before 5-8x minification on the
    # 400x240 display. Native high-resolution capture includes this integration;
    # a single bilinear lookup aliases the narrow white band. World bounds stay
    # authored; 20 texels/unit was checked against both saved native hit poses.
    size = tuple(min(bitmap.size[i], max(1, math.ceil(
        (bounds[i+2]-bounds[i])*FLASH_TEXELS_PER_UNIT))) for i in range(2))
    return bitmap.convert('RGBa').resize(size, Image.Resampling.BOX).convert('RGBA')


def nail_transform(read):
    """Activation transform before any post-Spawn caller assignments."""
    fsm = decode_fsm(read('resources.assets-23926.json')['m_Structure']['fsm'])
    state = next(s for s in fsm['states'] if s['name'] == 'Effect')
    actions = {a['type'].rsplit('.', 1)[-1]: a['parameters']
               for a in state['actions'] if a['enabled']}
    scale, rotation = actions['SetScale'], actions['SetRandomRotation']
    random = actions['RandomFloat']
    if (scale['x'] != {'variable': 'Randomiser'} or scale['y'] != scale['x'] or
            random['storeResult'] != scale['x'] or
            [rotation[k]['value'] for k in ('x', 'y', 'z')] != [False, False, True]):
        raise ValueError('Unsupported Strike Nail R transform actions')
    # SetRandomRotation.DoRandomRotation uses integer Random.Range(0, 360).
    return [random['min']['value'], random['max']['value'], 0, 360]


def debris_bases(transforms, shape, angle, dimensions=2, radius_scaled=True):
    """Radius-scaled XY columns after local SetRotationZ, then all parents.

    Order: neutral (up/down), left, right. Start's root-scale sign is already
    included in angle. Sampling and initial velocity are separate concerns.
    """
    if shape['m_Type'] != 10 or any(
            shape[k][f'm_{a}'] != expected for k, expected in
            [('m_Position', 0), ('m_Rotation', 0), ('m_Scale', 1)] for a in 'XYZ'):
        raise ValueError('Unsupported debris shape transform')
    q = transforms[0]['m_LocalRotation']
    if abs(q['m_X']) > 1e-6 or abs(q['m_Y']) > 1e-6:
        raise ValueError('Unsupported nonplanar debris local rotation')
    local = 2*math.atan2(q['m_Z'], q['m_W'])
    radius = shape['m_RadiusParameter']['m_Value'] if radius_scaled else 1
    bases = []
    for offset in (0, -angle, angle):
        chain = copy.deepcopy(transforms)
        a = local + math.radians(offset)
        chain[0]['m_LocalRotation'] = dict(m_X=0, m_Y=0, m_Z=math.sin(a/2), m_W=math.cos(a/2))
        origin = project((0, 0, 0), chain)
        for point in ((radius, 0, 0), (0, radius, 0)):
            bases.extend(v-o for v, o in zip(project(point, chain)[:dimensions], origin[:dimensions]))
    return bases


def cone_projection(shape, scale):
    """XY projection of the authored cone after Breakable.Hit's Euler rotation.

    Approximation: uniform disk sampling and no subsequent Z motion; no native
    Unity random sequence or cone sampling algorithm is claimed.
    """
    if shape['m_Type'] != 4 or shape['m_RadiusThickness'] != 1:
        raise ValueError('Unsupported dust cone')
    if any(shape[k][f'm_{a}'] != expected for k, expected in
           [('m_Position', 0), ('m_Rotation', 0), ('m_Scale', 1)] for a in 'XYZ'):
        raise ValueError('Unsupported dust shape transform')
    euler = (0, 90, 270)  # Regular dust, right hit (Breakable.Hit).
    def rotate(v):
        x, y, z = v
        rx, ry, rz = map(math.radians, euler)
        # Unity Euler order: Z, X, Y.
        x, y = x*math.cos(rz)-y*math.sin(rz), x*math.sin(rz)+y*math.cos(rz)
        y, z = y*math.cos(rx)-z*math.sin(rx), y*math.sin(rx)+z*math.cos(rx)
        x, z = x*math.cos(ry)+z*math.sin(ry), -x*math.sin(ry)+z*math.cos(ry)
        # Runtime rotates the right-facing basis by the strike direction.
        return (x, y)
    x, y, z = [rotate(v) for v in [(1,0,0), (0,1,0), (0,0,1)]]
    radius = shape['m_RadiusParameter']['m_Value']
    return [math.tan(math.radians(shape['m_Angle'])),
            *(v*radius*scale['m_X'] for v in x),
            *(v*radius*scale['m_Y'] for v in y), *x, *y, *z]


def grass_cone_projection(shape, transforms):
    """Project the grass cone through its child transform into the owner plane."""
    if shape['m_Type']!=4 or shape['m_RadiusThickness']!=1:
        raise ValueError('Unsupported grass particle cone')
    if any(shape[k][f'm_{a}']!=expected for k,expected in
           [('m_Position',0),('m_Rotation',0),('m_Scale',1)] for a in 'XYZ'):
        raise ValueError('Unsupported grass particle cone transform')
    origin=project((0,0,0),transforms)
    axes=[tuple(v-o for v,o in zip(project(point,transforms),origin))
          for point in ((1,0,0),(0,1,0),(0,0,1))]
    radius=shape['m_RadiusParameter']['m_Value']
    x,y,z=axes
    return [math.tan(math.radians(shape['m_Angle'])),
            x[0]*radius,x[1]*radius,y[0]*radius,y[1]*radius,
            x[0],x[1],y[0],y[1],z[0],z[1]]


def curve(c, t):
    """Evaluate serialized Hermite keys, without inventing scalar defaults."""
    keys = c['m_Curve']
    if not keys:
        raise ValueError('Empty active curve')
    if t <= keys[0]['m_Time']:
        return keys[0]['m_Value']
    for a, b in zip(keys, keys[1:]):
        if t <= b['m_Time']:
            dt = b['m_Time'] - a['m_Time']
            u = (t-a['m_Time']) / dt
            return ((2*u**3-3*u*u+1)*a['m_Value'] +
                    (u**3-2*u*u+u)*dt*a['m_OutSlope'] +
                    (-2*u**3+3*u*u)*b['m_Value'] +
                    (u**3-u*u)*dt*b['m_InSlope'])
    return keys[-1]['m_Value']


def limits(c, t=0):
    mode = c['m_MinMaxState']
    if mode == 0:
        return [c['m_Scalar']]*2
    if mode == 3:
        return [c['m_MinScalar'], c['m_Scalar']]
    hi = curve(c['m_MaxCurve'], t)*c['m_Scalar']
    lo = curve(c['m_MinCurve'], t)*c['m_MinScalar'] if mode == 2 else hi
    return [lo, hi]


def particle_alpha(module, t, mean_two_gradients=True):
    """Mean authored alpha, including both gradients for TwoGradients.

    Approximation: the package keeps eight mean samples instead of the original
    per-particle gradient blend. Using only MaxGradient biases dust opacity high.
    """
    if not module['m_Enabled']:
        return 1
    gradient = module['m_Gradient']
    mode = gradient['m_MinMaxState']
    def sample(g):
        keys = sorted((g[f'm_Atime{i}']/65535, g[f'm_Key{i}']['m_A'])
                      for i in range(g['m_NumAlphaKeys']))
        if not keys:
            raise ValueError('Empty active alpha gradient')
        if t <= keys[0][0]:
            return keys[0][1]
        for a, b in zip(keys, keys[1:]):
            if t <= b[0]:
                u = (t-a[0])/max(1e-9, b[0]-a[0])
                return a[1]+u*(b[1]-a[1])
        return keys[-1][1]
    if mode == 1:
        return sample(gradient['m_MaxGradient'])
    if mode == 3 and mean_two_gradients:
        return (sample(gradient['m_MinGradient'])+sample(gradient['m_MaxGradient']))*.5
    if mode == 3:
        return sample(gradient['m_MaxGradient'])
    raise ValueError('Unsupported particle alpha mode: '+str(mode))


def sprite_material_color(material, shader, ambient=(1,1,1)):
    """Use the active sprite shader contract, not stale serialized properties.

    Sprites/Lit has an empty program and falls back to Sprites/Diffuse in the
    saved export. Its vertex stage multiplies _Color and _RendererColor; the
    unrelated particle-shader _TintColor survives in Material serialization.
    A supplied flat ambient RGB is baked into the tint, leaving alpha unchanged.
    """
    form = shader['m_ParsedForm']
    if (form['m_Name'] != 'Sprites/Lit' or
            form['m_FallbackName'] != 'Sprites/Diffuse'):
        raise ValueError('Unsupported particle sprite shader')
    props = {p['m_Name']: p for p in form['m_PropInfo']['m_Props']}
    colors = material['m_Colors']
    result = [1.0]*4
    for name in ('_Color', '_RendererColor'):
        if name not in props or props[name]['m_Type'] != 0:
            raise ValueError('Missing sprite color property: '+name)
        prop = props[name]
        color = colors.get(name, {f'm_{c}': prop[f'm_DefValue_{i}_']
                                  for i,c in enumerate('RGBA')})
        result = [v*color[f'm_{c}'] for v,c in zip(result,'RGBA')]
    return [result[i]*ambient[i] for i in range(3)]+result[3:]


def room_force_curves(force):
    if not force['m_Enabled']:
        return None
    if not force['m_InWorldSpace'] or force.get('m_RandomizePerFrame', False):
        raise ValueError('Unsupported room particle force space/randomization')
    # Approximation: eight Hermite-curve samples, with original per-axis random
    # weights retained for each particle throughout its lifetime.
    return [limits(force['m_' + axis], i/7)[bound]
            for axis in 'XYZ' for bound in range(2) for i in range(8)]


def room_particle_material(material, shader, ambient=(1, 1, 1)):
    """The same source shader rules and RoomMaterial modes as scene particles."""
    name = shader['m_ParsedForm']['m_Name']
    colors = material['m_Colors']
    if name == 'Legacy Shaders/Particles/Additive (Soft)':
        # This shader has no tint property; serialized _TintColor is stale.
        return [1.0] * 4, 1  # RoomMaterial::Screen
    if name == 'Legacy Shaders/Particles/Additive':
        return [colors['_TintColor']['m_' + c] for c in 'RGBA'], 4
    if name == 'Sprites/Default':
        tint = colors.get('_Color', dict(m_R=1, m_G=1, m_B=1, m_A=1))
        renderer = colors.get('_RendererColor', dict(m_R=1, m_G=1, m_B=1, m_A=1))
        return [tint['m_' + c] * renderer['m_' + c] for c in 'RGBA'], 0
    return sprite_material_color(material, shader, ambient), 0


def grass_particle_source():
    profile_path=GRASS_SOURCE/'grass-2874-profile.json'
    texture_path=GRASS_SOURCE/'resources.assets-290.png'
    profile=json.loads(profile_path.read_text())
    source=profile['provenance']['source_assets_sha256']
    if (profile['schema'],profile['build'],profile['scene'],profile['owner'])!=(1,22529139,'Tutorial_01',2874):
        raise ValueError('Unexpected grass particle source profile')
    # Pin the decoded pixels, not the PNG bytes: the zlib build differs between platforms
    # (the Windows encode is baf43e24...); restore_decor_research pins the same pixels.
    with Image.open(texture_path) as opened:
        image=opened.convert('RGBA')
    if image.size!=(46,138) or hashlib.sha256(image.tobytes()).hexdigest()!='f9846af320cc09556d7346e27e55e3a56734c5d8e86dd165098f088fdde8a579':
        raise ValueError('Changed grass particle texture')
    if source.get('resources.assets')!='6d49ac5a50a282a645acc32518b28e7a29d0b94a0c3b5a4cda18dade3e6fe511':
        raise ValueError('Changed grass particle asset source')
    return profile,texture_path


def _hfx_fields(value):
    if isinstance(value,dict):
        return {(k if k.startswith('m_') else 'm_'+k[0].upper()+k[1:]):_hfx_fields(v)
                for k,v in value.items()}
    if isinstance(value,list):return [_hfx_fields(v) for v in value]
    return value


def grass_particle_config(profile):
    """Adapt the saved UnityPy type tree to the converter's existing HFX parser."""
    src=profile['system'];system=_hfx_fields(src)
    shape=system['m_ShapeModule']
    shape['m_RadiusParameter']={'m_Value':src['ShapeModule']['radius']['value']}
    shape['m_ArcParameter']={'m_Value':src['ShapeModule']['arc']['value']}
    system['m_Looping']=src['looping'];system['m_LengthInSec']=src['lengthInSec']
    system['m_ScalingMode']=src['scalingMode']
    return system


def emitter_factory(read, ref, chain, image, texture, images, identities, emitters, owner_source, plant_manifest, manifest, opening, stal, contact_source):
    """Compile original ParticleSystems through the shared decor model."""
    def emitter(owner, angle=None, ambient=(1,1,1), dust=False, grass=None, contact=False, allow_loop=False, owned_angle=None):
        material_mode = 0
        if grass:
            key=grass['system_key']
            p=grass_particle_config(grass)
            render=grass['renderer']
            tex='grass-particle-texture'
            bitmap=Image.open(GRASS_SOURCE/'resources.assets-290.png').convert('RGBA')
            tint=[1,1,1,1] # authored material and start color are both white
        else:
            if contact:
                components = contact_source.components(owner)
            else:
                objects = (owner_source.objects if owner in owner_source.objects else plant_manifest['objects'] if owner in plant_manifest['objects']
                          else manifest['derived']['objects'] if owner in manifest['derived']['objects']
                          else opening['objects'] if owner in opening['objects'] else stal['objects'])
                components = objects[owner]['components']
            key = components['ParticleSystem'][0]
            p = read(key)
            render = components['ParticleSystemRenderer'][0]
            material_key = ref(render,read(render)['m_Materials'][0])
            material_object = read(material_key)
            material = material_object['m_SavedProperties']
            shader_key = ref(material_key, material_object['m_Shader'])
            if owner=='level6-3218.json':
                if read(shader_key)['m_ParsedForm']['m_Name']!='Sprites/Default':
                    raise ValueError('Unexpected flock shader')
                color=material['m_Colors'].get('_Color',dict(m_R=1,m_G=1,m_B=1,m_A=1))
                tint=[color['m_'+c] for c in 'RGBA']
            elif owned_angle is not None:
                tint, material_mode = room_particle_material(material, read(shader_key), ambient)
            else:
                tint = sprite_material_color(material, read(shader_key), ambient)
            tex = ref(material_key, material['m_TexEnvs']['_MainTex']['m_Texture'])
            bitmap = texture(tex)
        initial, emission, shape = (p['m_InitialModule'],p['m_EmissionModule'],p['m_ShapeModule'])
        if (p['m_Looping'] and not allow_loop) or emission['m_Bursts']:
            raise ValueError('Unsupported looping/burst emitter: '+key)
        uv = p['m_UVModule']
        columns, rows = (uv['m_TilesX'],uv['m_TilesY']) if uv['m_Enabled'] else (1,1)
        first = len(images)
        for row in range(rows):
            for col in range(columns):
                w,h = bitmap.width//columns,bitmap.height//rows
                image((tex,col,row,columns,rows),bitmap.crop((col*w,row*h,(col+1)*w,(row+1)*h)),(-.5,-.5,.5,.5))
        first = identities[(tex,0,0,columns,rows)]
        if grass:
            t=grass['transform']
            transform={'m_LocalPosition':{'m_X':t['local_position']['x'],'m_Y':t['local_position']['y'],'m_Z':t['local_position']['z']},
                       'm_LocalScale':{'m_X':t['local_scale']['x'],'m_Y':t['local_scale']['y'],'m_Z':t['local_scale']['z']},
                       'm_LocalRotation':{'m_X':t['local_rotation']['x'],'m_Y':t['local_rotation']['y'],'m_Z':t['local_rotation']['z'],'m_W':t['local_rotation']['w']},
                       'm_Father':{'m_FileID':0,'m_PathID':0}}
            transforms=[transform]
        else:
            transform = components['Transform'][0]
            transforms = chain(transform)
            if owned_angle is not None:
                transforms = owner_source.instance_transform(owner, transforms)
        origin = project((0,0,0),transforms)
        px,py = project((1,0,0),transforms),project((0,1,0),transforms)
        rotation = math.atan2(px[1]-origin[1],px[0]-origin[0])
        arc = shape['m_ArcParameter']['m_Value']*math.pi/180
        if shape['m_Type']==10:  # Circle: authored arc starts at local +X.
            determinant=(px[0]-origin[0])*(py[1]-origin[1])-(px[1]-origin[1])*(py[0]-origin[0])
            arc=math.copysign(arc,determinant)
            rotation+=arc*.5
        scale = math.hypot(py[0]-origin[0],py[1]-origin[1])
        # ParticleSystemScalingMode.Shape (2) scales the emitter shape only.
        # In particular the rotated dust transform has a nearly zero projected
        # Y basis; applying that to billboard size made all of its dust invisible.
        if p['m_ScalingMode'] != 2 and not (p['m_ScalingMode']==1 and owner in ('level6-3897.json','level6-3850.json','level6-3874.json')) and owner!='level6-3218.json':
            raise ValueError('Unsupported particle scaling mode: '+key)
        def sampled(module, field, default):
            if not p[module]['m_Enabled']:
                return [default]*8
            # approximation: eight mean samples replace Unity per-particle curves.
            return [sum(limits(p[module][field],i/7))/2 for i in range(8)]
        color = initial['m_StartColor']['m_MaxColor']
        rate = limits(emission['m_RateOverTime'])[1] if emission['m_Enabled'] else 0
        count = min(initial['m_MaxNumParticles'],math.ceil(rate*p['m_LengthInSec']))
        force = p['m_ForceModule']
        if force['m_Enabled'] and (not force['m_InWorldSpace'] or
                (owned_angle is None and limits(force['m_X']) != [0,0] and owner not in opening['groups']['land']+['level6-2420.json'])):
            raise ValueError('Unsupported force space/X: '+key)
        clamp = p['m_ClampVelocityModule']
        collision = p['m_CollisionModule']
        values = [*origin,rotation,arc,
                  shape['m_RadiusParameter']['m_Value']*scale,p['m_LengthInSec'],
                  *limits(initial['m_StartLifetime']),*limits(initial['m_StartSpeed']),
                  *limits(initial['m_StartSize']),*limits(initial['m_StartRotation']),
                  limits(initial['m_GravityModifier'])[1],
                  *(limits(force['m_Y']) if force['m_Enabled'] else [0,0]),
                  clamp['m_Dampen'] if clamp['m_Enabled'] else 0,
                  limits(clamp['m_Magnitude'])[1] if clamp['m_Enabled'] else 1e6,
                  limits(collision['m_Bounce'])[1] if collision['m_Enabled'] else 0,
                  collision['m_MinKillSpeed'] if collision['m_Enabled'] else 0]
        values += sampled('m_SizeModule','m_Curve',1)
        values += [particle_alpha(p['m_ColorModule'],i/7,dust) for i in range(8)]
        values += sampled('m_RotationModule','m_Curve',0)
        values += [color[f'm_{c}']*v for c,v in zip('RGBA',tint)]
        velocity=p['m_VelocityModule']
        if owner in ('level6-3218.json','level6-3874.json') and velocity['m_Enabled'] and not velocity['m_InWorldSpace']:
            velocity=copy.deepcopy(velocity)
            # Source flock/final floor bits have local X drift only. Project it through
            # its authored transform; XY RNG correlation is approximated.
            if limits(velocity['m_Y'])!=[0,0] or limits(velocity['m_Z'])!=[0,0]:
                raise ValueError('Unexpected local X drift: '+key)
            drift=limits(velocity['m_X']);axis=[px[i]-origin[i] for i in range(3)]
            for component,direction in zip(('m_X','m_Y','m_Z'),axis):
                velocity[component]=dict(m_MinMaxState=3,m_MinScalar=drift[0]*direction,m_Scalar=drift[1]*direction)
            velocity['m_InWorldSpace']=True
        for axis in ['m_X','m_Y']:
            if velocity['m_Enabled']:
                if not velocity['m_InWorldSpace'] or velocity[axis]['m_MinMaxState'] not in (0,3):
                    raise ValueError('Unsupported active velocity module: '+key)
                values += limits(velocity[axis])
            else:
                values += [0,0]
        if velocity['m_Enabled'] and velocity['m_Z']['m_MinMaxState'] not in (0,3):
            raise ValueError('Unsupported Z velocity module: '+key)
        values += limits(force['m_Z']) if force['m_Enabled'] else [0,0]
        values += limits(velocity['m_Z']) if velocity['m_Enabled'] else [0,0]
        values += [limits(collision['m_Dampen'])[1] if collision['m_Enabled'] else 0,
                   collision['m_RadiusScale'] if collision['m_Enabled'] else 0]
        assert len(values)==60
        cone = (cone_projection(shape, transforms[0]['m_LocalScale'])
                if owner=='resources.assets-6956.json' else [0]*11)
        if grass:
            # approximation: project the authored cone through the ParticleSystem
            # child transform; the owner matrix is applied when the cut is emitted.
            cone=grass_cone_projection(shape,transforms)
        if owner in opening['groups']['land']+opening['groups']['flock']:
            # Approximation: projected uniform cone for the native Cone/ConeShell
            # shapes, retaining the authored transform flattening and direction.
            projected=copy.deepcopy(shape)
            if projected['m_Type']==12:projected['m_Type']=4
            cone=grass_cone_projection(projected,transforms)
        if owner in ('level6-350.json','level6-349.json'):
            # approximation: the ConeShell release emitter uses a projected
            # disk/cone, retaining its authored shape scale and angle.
            projected=copy.deepcopy(shape);projected['m_Type']=4
            projected['m_Scale']={'m_X':1,'m_Y':1,'m_Z':1}
            shape_transform={'m_LocalScale':shape['m_Scale'],
                'm_LocalPosition':shape['m_Position'],
                'm_LocalRotation':{'m_X':0,'m_Y':0,'m_Z':0,'m_W':1}}
            cone=grass_cone_projection(projected,[shape_transform,*transforms])
            values[5]=shape['m_RadiusParameter']['m_Value']
        bases = debris_bases(transforms, shape, angle) if angle is not None else [0]*12
        axes = debris_bases(transforms, shape, angle, 3, False) if angle is not None else [0]*18
        if owned_angle is not None:
            kind = shape['m_Type']
            if kind in (5, 10, 12):
                bases, axes = [], []
                # Source Box / SingleSidedEdge: transform the volume and its
                # local +Z emission direction after Breakable's local Z turn.
                for turn in (0, -owned_angle, owned_angle):
                    matrix = copy.deepcopy(transforms)
                    q = matrix[0]['m_LocalRotation']
                    a = math.radians(turn)/2
                    c, s = math.cos(a), math.sin(a)
                    matrix[0]['m_LocalRotation'] = dict(m_X=c*q['m_X']+s*q['m_Y'], m_Y=c*q['m_Y']-s*q['m_X'],
                                                       m_Z=c*q['m_Z']+s*q['m_W'], m_W=c*q['m_W']-s*q['m_Z'])
                    pivot = project((0,0,0), matrix)
                    if kind == 10:
                        for point in ((1,0,0),(0,1,0)):
                            column = [v-o for v,o in zip(project(point,matrix),pivot)]
                            bases.extend(v*shape['m_RadiusParameter']['m_Value'] for v in column[:2])
                            axes.extend(column)
                        continue
                    direction = project((0,0,1), matrix)
                    direction = [v-o for v,o in zip(direction,pivot)]
                    length = math.sqrt(sum(v*v for v in direction))
                    bases.extend(v/length for v in direction)
                    for axis in range(3):
                        point = [0,0,0]
                        point[axis] = (shape['m_Scale']['m_'+'XYZ'[axis]] if kind == 5
                                       else 2*shape['m_RadiusParameter']['m_Value'] if axis == 0 else 0)
                        axes.extend(v-o for v,o in zip(project(point,matrix)[:2],pivot[:2]))
                bases += [0]*(12-len(bases))
            else:
                raise ValueError('Unsupported owned emitter shape: '+key)
        if contact:
            if shape['m_Type']!=5:raise ValueError('Contact particles require source Box shape')
            # Instantiate replaces the prefab's saved scene position with the
            # platform position. Keep its shape basis, not that absolute offset.
            values[0:2]=[0,0]
            # Project the authored Box volume, including child rotation and scale.
            # Native depth/speed sampling is approximated by the shared XY model.
            box_columns=[]
            for axis in 'XYZ':
                vector=[0,0,0];vector['XYZ'.index(axis)]=shape['m_Scale'][f'm_{axis}']
                point=project(vector,transforms)
                box_columns.extend([point[0]-origin[0],point[1]-origin[1]])
            bases[:6]=box_columns
            axes[6:8]=limits(initial['m_GravityModifier'])
            axes[8:10]=limits(emission['m_RateOverTime'])
            values[5]=0
        if grass:
            key=grass['system_key']
        else:
            renderer=read(render)
        result = dict(source=key,owner=owner,sprite=first,tiles=columns*rows,count=count,rate=rate,values=values,cone=cone,bases=bases,axes=axes,
                      force_x=limits(force['m_X']) if force['m_Enabled'] else [0,0],
                      sorting=([render['m_SortingLayer'],render['m_SortingOrder']] if grass else [renderer['m_SortingLayer'], renderer['m_SortingOrder']]))
        if owned_angle is not None:
            result.update(shape=shape['m_Type'], visual=dust, material=material_mode, force_curves=room_force_curves(force))
        emitters.append(result)
        return len(emitters)-1
    return emitter


def observe(source=SOURCE):
    contact_source = ContactSource()
    owner_source = OwnerSource()
    manifest = json.loads((source/'decor-effects.json').read_text())
    initial_source=ROOT/'_private/research/initial-fall'
    opening=json.loads((initial_source/'initial-fall.json').read_text())
    for key,expected in opening['source_sha256'].items():
        if hashlib.sha256((initial_source/key).read_bytes()).hexdigest()!=expected:
            raise ValueError('Changed initial fall dependency: '+key)
    stal_source=ROOT/'_private/research/stalactite-sources'
    stal=json.loads((stal_source/'stalactite-sources.json').read_text())
    if (stal['build'],stal['scene'])!=(22529139,'Tutorial_01'):
        raise ValueError('Unexpected stalactite source build/scene')
    for key,expected in stal['source_sha256'].items():
        if hashlib.sha256((stal_source/key).read_bytes()).hexdigest()!=expected:
            raise ValueError('Changed stalactite dependency: '+key)
    plant_manifest = json.loads((PLANT_SOURCE/'plant-effects.json').read_text())
    if (plant_manifest['build'],plant_manifest['scene']) != (22529139,'Tutorial_01'):
        raise ValueError('Unexpected plant effects build/scene')
    # Verify all input resources, including media, before using them.
    for key, expected in manifest['source_sha256'].items():
        if hashlib.sha256((source/key).read_bytes()).hexdigest() != expected:
            raise ValueError('Changed dependency: '+key)
    for key, expected in plant_manifest['source_sha256'].items():
        if hashlib.sha256((PLANT_SOURCE/key).read_bytes()).hexdigest() != expected:
            raise ValueError('Changed plant dependency: '+key)
    room = ROOT/'_private/research/room-visuals'
    room_manifest = json.loads((room/'room-visuals-map.json').read_text())
    if hashlib.sha256((room/'room-visuals-map.json').read_bytes()).hexdigest()!=plant_manifest['room_manifest_sha256']:
        raise ValueError('Changed plant room manifest')
    extra_hashes = {}
    def read(k):
        if owner_source.has(k):
            return owner_source.read(k)
        path = source/k
        if not path.exists():
            if contact_source.has(k):
                return contact_source.read(k)
            if k in opening['source_sha256']:
                path=initial_source/k
            elif k in stal['source_sha256']:
                path=stal_source/k
            elif k in plant_manifest['source_sha256']:
                path = PLANT_SOURCE/k
            else:
                path = room/k
                expected = room_manifest['source_sha256'][k]
                if hashlib.sha256(path.read_bytes()).hexdigest()!=expected:
                    raise ValueError('Changed room ancestor: '+k)
                extra_hashes[k]=expected
        return json.loads(path.read_text())
    def ref(k, v):
        table = (owner_source.manifest['dependency_tables'].get(k) or manifest['dependency_tables'].get(k)
                 or stal['dependency_tables'].get(k) or opening['dependency_tables'].get(k))
        if table:
            collection = table[str(v['m_FileID'])]
        elif contact_source.has(k):
            if v['m_FileID']!=0:raise ValueError('Nonlocal contact reference: '+k)
            collection=k.rsplit('-',1)[0]
        elif k in plant_manifest['source_sha256']:
            collection = {0:k.rsplit('-',1)[0],2:'sharedassets6.assets',4:'resources.assets'}[v['m_FileID']]
        else:
            if v['m_FileID']!=0:raise ValueError('Nonlocal room ancestor')
            collection = k.rsplit('-',1)[0]
        return collection+'-'+str(v['m_PathID'])+'.json'
    def chain(k):
        result = []
        while k:
            t = read(k)
            result.append(t)
            k = ref(k,t['m_Father']) if t['m_Father']['m_PathID'] else None
        return result
    def root_transform(owner):
        go = read(owner)
        for c in go['m_Components']:
            k = ref(owner,c['m_Component'])
            if 'm_LocalPosition' in read(k):
                return k
        raise ValueError('Missing Transform '+owner)
    images, geometry, identities = [], [], {}
    def image(identity, bitmap, bounds):
        if identity not in identities:
            identities[identity] = len(images)
            images.append(bitmap)
            geometry.append(bounds)
        return identities[identity]
    def texture(k):
        if k in owner_source.manifest['media']:
            return owner_source.texture(k)
        if k in plant_manifest['textures']:
            return Image.open(PLANT_SOURCE/plant_manifest['textures'][k]).convert('RGBA')
        if k in stal['media']:
            return Image.open(stal_source/stal['media'][k]['png']).convert('RGBA')
        if k in opening['media']:
            return Image.open(initial_source/opening['media'][k]['png']).convert('RGBA')
        if k not in manifest['media']:
            return contact_source.texture(k)
        return Image.open(source/manifest['media'][k]['png']).convert('RGBA')
    emitters = []
    emitter = emitter_factory(read, ref, chain, image, texture, images, identities, emitters, owner_source, plant_manifest, manifest, opening, stal, contact_source)
    profiles=[]
    for record in manifest['derived']['records']:
        root = f"level6-{record['owner']}.json"
        origin = project((0,0,0),chain(root_transform(root)))
        data = read(record['breakable'])['m_Structure']
        root_scale = read(root_transform(root))['m_LocalScale']['m_X']
        signed_angle = data['angleOffset'] * (1 if root_scale > 0 else -1 if root_scale < 0 else 0)
        ids = [emitter(k, signed_angle) for k in record['debris']]
        ids += [0xffffffff]*(2-len(ids))
        offset=data['effectOffset']
        profiles.append(dict(owner=record['owner'],emitters=ids,position=origin,
                             offset=[offset['x'],offset['y']],angle=data['angleOffset']))
    dust=[emitter(k, ambient=scene_ambient(), dust=True)
          for k in ['resources.assets-6956.json','resources.assets-3947.json']]
    flashes=[]
    library=read('resources.assets-21298.json')['m_Structure']
    strike_scale=read('resources.assets-9183.json')['m_LocalScale']
    # The FSM tests its persisted Chooser before RandomBool. A fresh instance
    # starts false (Anim 2); the animator's defaultClipId=3 is overridden.
    fsm=read('resources.assets-23653.json')['m_Structure']['fsm']
    chooser=next(v['value'] for v in fsm['variables']['boolVariables'] if v['name']=='Chooser')
    choose=next(s for s in fsm['states'] if s['name']=='Choose Anim')
    state=next(t['toState'] for t in choose['transitions'] if t['fsmEvent']['name']==('TRUE' if chooser else 'FALSE'))
    animation=next(s for s in fsm['states'] if s['name']==state)
    clip_name=next(p['value'].strip() for p in animation['actionData']['fsmStringParams'] if not p['useVariable'])
    strike_clip=next(i for i,c in enumerate(library['clips']) if c['name']==clip_name)
    for clip_id,scale,renderer in [(strike_clip,(strike_scale['m_X'],strike_scale['m_Y']),'resources.assets-12084.json'),
                                   (0,(1.5,1.5),'resources.assets-12081.json')]:
        clip=library['clips'][clip_id]
        first=len(images)
        for frame in clip['frames']:
            k=ref('resources.assets-21298.json',frame['spriteCollection'])
            collection=read(k)['m_Structure']
            sprite=collection['spriteDefinitions'][frame['spriteId']]
            tex=ref(k,collection['textures'][sprite['materialId']])
            bitmap,bounds=decode_sprite(texture(tex),sprite)
            # Slash Impact prefab Transform; SpawnNailHitEffect sets nail XY=1.5.
            bounds=tuple(v*scale[i%2] for i,v in enumerate(bounds))
            bitmap=prefilter_flash(bitmap,bounds)
            image((k,frame['spriteId']),bitmap,bounds)
        render=read(renderer)
        flashes.append(dict(sprite=first,count=len(clip['frames']),fps=clip['fps'],clip=clip['name'],
                            sprite_ids=[f['spriteId'] for f in clip['frames']],
                            sorting=[render['m_SortingLayer'],render['m_SortingOrder']]))
    # Keep existing emitter and dust indices stable; plants append after shared dust.
    for plant in plant_manifest['plants']:
        root = f"level6-{plant['owner']}.json"
        origin = project((0,0,0),chain(root_transform(root)))
        data = read(plant['breakable'])['m_Structure']
        root_scale = read(root_transform(root))['m_LocalScale']['m_X']
        signed_angle = data['angleOffset']*(1 if root_scale>0 else -1 if root_scale<0 else 0)
        ids = [emitter(k,signed_angle) for k in plant['debris']]
        offset = data['effectOffset']
        profiles.append(dict(owner=plant['owner'],emitters=ids,position=origin,
                             offset=[offset['x'],offset['y']],angle=data['angleOffset']))
    grass=None
    if (GRASS_SOURCE/'grass-2874-profile.json').exists():
        grass_source,grass_texture=grass_particle_source()
        uv=grass_source['system']['UVModule']
        if (uv['tilesX'],uv['tilesY'],uv['animationType'],uv['rowMode'])!=(1,3,1,1):
            raise ValueError('Unsupported grass random-row texture animation')
        index=emitter('grass',grass=grass_source)
        # Source float duration is 0.10000000149; avoid a phantom 26th birth.
        emitters[index]['count']=round(emitters[index]['rate']*emitters[index]['values'][6])
        grass=dict(emitter=index,count=emitters[index]['count'],animation_fps=0,
                   texture_sha256=hashlib.sha256(grass_texture.read_bytes()).hexdigest())
    contact = compile_bank(contact_source,image,emitter,read,chain,root_transform)
    # Keep the existing profile indices stable. The eight source copies share
    # one set of modules; offsets are relative to their moving body.
    stal_ids=[];stal_offsets=[]
    root_origin=project((0,0,0),chain('level6-4229.json'))
    for owner in ['level6-350.json','level6-349.json','level6-353.json','resources.assets-6083.json','level6-3842.json']:
        index=emitter(owner,ambient=scene_ambient(),dust=owner!='level6-3842.json',allow_loop=owner=='level6-353.json')
        stal_ids.append(index)
        origin=emitters[index]['values'][:3]
        stal_offsets.append([origin[j]-(root_origin[j] if owner.startswith('level6') else 0) for j in range(3)])
    animator=read('resources.assets-21812.json')['m_Structure']
    thunk_clip=library['clips'][animator['defaultClipId']]
    first=len(images)
    for frame in thunk_clip['frames']:
        key=ref('resources.assets-21298.json',frame['spriteCollection']);collection=read(key)['m_Structure']
        sprite=collection['spriteDefinitions'][frame['spriteId']]
        tex=ref(key,collection['textures'][sprite['materialId']])
        bitmap,bounds=decode_sprite(texture(tex),sprite)
        scale=read('resources.assets-9473.json')['m_LocalScale']
        bounds=tuple(v*scale['m_X' if j%2==0 else 'm_Y'] for j,v in enumerate(bounds))
        image(('stalactite-thunk',len(images)),prefilter_flash(bitmap,bounds),bounds)
    thunk_renderer=read('resources.assets-12170.json')
    stal_fx=dict(emitters=stal_ids,offsets=stal_offsets,thunk=dict(sprite=first,count=len(thunk_clip['frames']),fps=thunk_clip['fps'],
        sorting=[thunk_renderer['m_SortingLayer'],thunk_renderer['m_SortingOrder']]))
    initial_ids=[emitter(k,ambient=scene_ambient(),dust=True) for k in opening['groups']['land']]
    initial_ids += [emitter(k) for k in opening['groups']['flock']]
    if len(initial_ids)!=3:
        raise ValueError('Expected two initial impact dust systems and one flock')
    # Main's contact art and the opening birds share the compact resident page.
    # Empty flock margins need no texels; keep every visible pixel at its source position.
    flock=emitters[initial_ids[2]]
    for i in range(flock['sprite'],flock['sprite']+flock['tiles']):
        images[i],geometry[i]=trim_transparent(images[i],geometry[i])
    line_parts=opening['objects']['resources.assets-6728.json']['components']
    animator=read(line_parts['tk2dSpriteAnimator'][0])['m_Structure']
    lk=ref(line_parts['tk2dSpriteAnimator'][0],animator['library'])
    line_clip=read(lk)['m_Structure']['clips'][animator['defaultClipId']]
    first=len(images)
    line_transform=read(line_parts['Transform'][0])
    for n,frame in enumerate(line_clip['frames']):
        ck=ref(lk,frame['spriteCollection']);collection=read(ck)['m_Structure']
        sprite=collection['spriteDefinitions'][frame['spriteId']]
        tex=ref(ck,collection['textures'][sprite['materialId']])
        bitmap,bounds=decode_sprite(texture(tex),sprite)
        bounds=tuple(v*line_transform['m_LocalScale']['m_X' if j%2==0 else 'm_Y'] for j,v in enumerate(bounds))
        image(('initial-impact-lines',n),prefilter_flash(bitmap,bounds),bounds)
    uv=read(emitters[initial_ids[2]]['source'])['m_UVModule']
    initial_fx=dict(emitters=initial_ids,offsets=[emitters[i]['values'][:3] for i in initial_ids],
                    sorting=[emitters[i]['sorting'] for i in initial_ids],flock_cycles=uv['m_Cycles'],
                    force_x=[emitters[i]['force_x'] for i in initial_ids],
                    lines=dict(sprite=first,count=len(line_clip['frames']),fps=line_clip['fps'],
                               offset=project((0,0,0),chain(line_parts['Transform'][0]))))
    owner_fx = compile_owners(owner_source, emitter, emitters, chain, root_transform, project, read, scene_ambient())
    stal_bound=8*(sum(emitters[i]['count'] for i in stal_ids if i!=stal_ids[2])+
        math.ceil(emitters[stal_ids[2]]['rate']*emitters[stal_ids[2]]['values'][8])+12+3+max(emitters[i]['count'] for i in dust))
    bound=sum(emitters[i]['count'] for profile in profiles for i in profile['emitters']
              if i != 0xffffffff)+len(profiles)*(max(emitters[i]['count'] for i in dust)+2)
    if grass:
        bound+=39*(grass['count']+1) # interactive owners and their Slash Impact
    bound+=stal_bound+sum(emitters[i]['count'] for i in initial_ids)+7
    bound+=18*sum(emitters[i]['count'] for i in contact['emitters'])
    bound+=owner_fx['pool_bound']+11 # Break Floor 1 FlingObjects: 3 + 8 rubble bodies
    # Seven scene HealthManagers, Shade, Grimmkin, two Health Scuttlers and
    # cocoon (Sword + Slash); chest, break floor and Mr Mushroom (Sword).
    hit_streak_bound=2*(7+2+2+1)+3
    bound+=hit_streak_bound
    if bound>POOL:
        raise ValueError(f'Effect pool budget exceeded: {bound} > {POOL}')
    for i in dust:
        p=read(emitters[i]['source'])
        if p['m_EmissionModule']['m_RateOverTime']['m_MinMaxState'] != 0:
            raise ValueError('Timed dust requires an authored constant rate')
    table=manifest['derived']['audio_tables']['sharedassets6.assets-1059.json']
    return dict(emitters=emitters,profiles=profiles,dust=dust,flashes=flashes,nail_transform=nail_transform(read),pool_bound=bound,hit_streak_pool_bound=hit_streak_bound,grass=grass,contact=contact,owner_fx=owner_fx,
                initial_fall=initial_fx,initial_fall_source_sha256=opening['source_sha256'],
                stalactite_fx=stal_fx,stalactite_pool_bound=stal_bound,stalactite_effect_source_sha256=stal['source_sha256'],
                audio_pitch=[table['pitchMin'],table['pitchMax']],
                images=images,geometry=geometry,source_sha256=manifest['source_sha256'],
                plant_source_sha256=plant_manifest['source_sha256'],room_ancestors_sha256=extra_hashes,
                dust_ambient_rgb=scene_ambient(),scene_light_sha256=SCENE_LIGHT_SHA256)


def convert(romfs=None):
    data=observe()
    from great_door_artwork import append as append_great_door
    append_great_door(data,ROOT)
    if data['pool_bound']>POOL:
        raise ValueError('Great Door exceeds the fixed effect pool')
    output=ROOT/'_private/converted/decor-effects'
    romfs=Path(romfs) if romfs is not None else ROOT/'_private/converted/hero/romfs'
    output.mkdir(parents=True,exist_ok=True)
    (output/'owner-effects.bin').write_bytes(data['owner_fx'].pop('binary'))
    shutil.copyfile(output/'owner-effects.bin',romfs/'owner-effects.bin')
    contact_binary=data['contact'].pop('binary')
    (output/'contact-effects.bin').write_bytes(contact_binary)
    from stalactite_artwork import prepare as prepare_stalactites
    prepare_stalactites(data,output,romfs)
    initial=data['initial_fall'];lines=initial['lines']
    initial_binary=struct.pack('<4s3I9f6f6ff2If3f',b'IFX1',*initial['emitters'],
                               *(x for v in initial['offsets'] for x in v),
                               *(x for v in initial['sorting'] for x in v),
                               *(x for v in initial['force_x'] for x in v),initial['flock_cycles'],
                               lines['sprite'],lines['count'],lines['fps'],*lines['offset'])
    (output/'initial-fall-fx.bin').write_bytes(initial_binary)
    shutil.copyfile(output/'initial-fall-fx.bin',romfs/'initial-fall-fx.bin')
    fx=data['stalactite_fx'];thunk=fx['thunk']
    fx_binary=struct.pack('<4s5I15f2I3f',b'STF1',*fx['emitters'],*(x for v in fx['offsets'] for x in v),
                          thunk['sprite'],thunk['count'],thunk['fps'],*thunk['sorting'])
    (output/'stalactite-fx.bin').write_bytes(fx_binary)
    shutil.copyfile(output/'stalactite-fx.bin',romfs/'stalactite-fx.bin')
    # Admission: one compact RGBA4 512 page (512 KiB), allocated during load.
    # Append the Unity mote to this page so ambient particles share the resident
    # decor texture allocation and placement table.
    ambient_sprite=len(data['images'])
    data['images'].append(ambient_artwork())
    data['geometry'].append((-.5,-.5,.5,.5))
    data['images'].append(ambient_artwork(soft=True))
    data['geometry'].append((-.5,-.5,.5,.5))
    deepnest_banks={}
    deepnest_manifest=ROOT/'_private/research/deepnest-ambient-particles/ambient-particles.json'
    if deepnest_manifest.is_file():
        deepnest_banks=append_deepnest_banks(data['images'],data['geometry'])
    sprites=[(i,bitmap,bounds) for i,(bitmap,bounds) in enumerate(zip(data.pop('images'),data.pop('geometry')))]
    # Shared breakable art and recovered ambient sheets must fit together.
    # The existing renderer supports multiple pages; retain a two-page bound.
    records,pages=pack_pages(sprites,[(512,512)]*2)
    pages=pages[:1+max(record[0] for record in records.values())]
    placements=[records[i] for i in range(len(sprites))]
    visual=bytearray(struct.pack('<4s4I',b'HKR2',len(pages),len(placements),3,0))
    for placement in placements:visual.extend(struct.pack('<5I4f',*placement))
    # Invisible templates carry dust, strike and nail renderer sorting keys.
    # HFX7 also carries the shared nail prefab's activation transform.
    dust_sorting = data['emitters'][data['dust'][0]]['sorting']
    if dust_sorting != data['emitters'][data['dust'][1]]['sorting'] or dust_sorting[0] <= 0:
        raise ValueError('Dust requires a shared foreground sorting layer')
    for sorting in [dust_sorting,*(f['sorting'] for f in data['flashes'])]:
        visual.extend(struct.pack('<2I16f',0,0,1,0,0,0,1,0,0,1,1,1,0,*sorting,0,0,0))
    (output/'decor-fx-visual.bin').write_bytes(visual)
    binary=bytearray(struct.pack('<4s4I',b'HFX7',len(data['emitters']),len(data['profiles']),*data['dust']))
    for e in data['emitters']:binary.extend(struct.pack('<3I102f',e['sprite'],e['tiles'],e['count'],*e['values'],e['rate'],*e['cone'],*e['bases'],*e['axes']))
    for p in data['profiles']:binary.extend(struct.pack('<3I6f',p['owner'],*p['emitters'],*p['position'],*p['offset'],p['angle']))
    for f in data['flashes']:binary.extend(struct.pack('<2If',f['sprite'],f['count'],f['fps']))
    binary.extend(struct.pack('<4f',*data['nail_transform']))
    binary.extend(struct.pack('<2f',*data['audio_pitch']))
    if data['grass']:
        g=data['grass']
        binary.extend(struct.pack('<4s2If',b'GRS1',g['emitter'],g['count'],g['animation_fps']))
    motion=data['great_door']
    binary.extend(struct.pack('<4s133f',b'DMX2',
                              *(v for row in motion['bodies']+motion['dust_alpha']+motion['dust_size']+[motion['start_delays']]+motion['hulls'] for v in row)))
    binary.extend(struct.pack('<4s7I6f',b'DFX1',*data['great_door']['emitters'],
                              *(v for pair in data['great_door']['sorting'] for v in pair)))
    (output/'decor-effects.bin').write_bytes(binary)
    (output/'ambient-particles.bin').write_bytes(compile_ambient_config(ambient_sprite))
    for zone,bank in deepnest_banks.items():
        name=f'deepnest-ambient-{zone}.bin'
        (output/name).write_bytes(bank)
    names=['decor-effects.bin','decor-fx-visual.bin','ambient-particles.bin','contact-effects.bin']
    for index,page in enumerate(pages):
        name=f'decor-fx-page-{index}'
        png=output/(name+'.png');page.save(png)
        tex3ds_cache.run([TEX3DS_EXE,'-f','rgba4','-z','none','-o',str(output/(name+'.t3x')),str(png)],check=True)
        names.append(name+'.t3x')
    names.extend(f'deepnest-ambient-{zone}.bin' for zone in deepnest_banks)
    for name in names:
        shutil.copyfile(output/name,romfs/name)
    for src,dst in [('sharedassets6.assets-92.wav','sfx-decor.wav'),('resources.assets-1343.wav','sfx-decor-nail.wav')]:
        shutil.copyfile(SOURCE/src,romfs/dst)
    data['approximations']=[
        'Grass uses authored 25 births, 0.7-1.3 s lifetime and three random texture rows; immediate burst and projected cone replace native scheduling and 3D simulation.',
        'Flash artwork uses premultiplied area minification at 20 texels/world-unit before RGBA4 packing; authored bounds, tint, timing and sorting stay unchanged.',
        'Sprite particle tint uses authored _Color * _RendererColor, not stale _TintColor. Dust bakes Tutorial SceneManager flat ambient; dynamic SH/point lights and debris lighting remain omitted.',
        'Debris remains an immediate bounded burst; dust uses fractional birth ages inside fixed steps and a total one-shot cap, not the native end-of-render scheduler.',
        'Dust alpha averages both authored ColorOverLifetime gradients; eight samples replace the original random per-particle blend. Debris alpha remains unchanged.',
        'Strike flash uses the fresh FSM Chooser state; pooled-instance Chooser reuse is not reproduced. Strike Nail R activation uses random uniform scale and integer Z rotation; post-Spawn caller overrides remain authoritative. Unity random sequence is approximated.',
        'Eight mean samples replace lifetime curves; debris birth and normalized velocity use post-hit bases, down dust uses the previous projected arc, regular dust uses a projected authored cone; native sampling is omitted.',
        'Measured full-speed clamp and separate force/gravity additions; horizontal terrain plane sampled at the owner replaces native CircleCast/manifold selection. No native contact solver or exact scheduler.',
        'Constant world velocity-over-lifetime is sampled once per particle; rotation-by-speed remains omitted.',
        'EnemyKillShake uses a short decaying displacement; CameraShake FSM was not exported.']
    data['ambient_particles']={'source':'_private/research/ambient-particles/ambient-particles.json',
                              'sprite':ambient_sprite,'config':'ambient-particles.bin',
                              'capacity':184,'samples_per_curve':16,'emission_density':1.0}
    if deepnest_banks:
        data['deepnest_ambient']={str(zone):f'deepnest-ambient-{zone}.bin' for zone in sorted(deepnest_banks)}
    data['particle_density']=1.0
    texture_bytes=len(pages)*512*512*2
    data.update(pages=len(pages),page_size=512,texture_format='rgba4',texture_bytes=texture_bytes)
    (output/'manifest.json').write_text(json.dumps(data,indent=2)+'\n')
    print(json.dumps(dict(profiles=len(data['profiles']),emitters=len(data['emitters']),sprites=len(placements),pool_bound=data['pool_bound'],texture_bytes=texture_bytes)))


if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--romfs',type=Path,default=ROOT/'_private/converted/hero/romfs')
    convert(parser.parse_args().romfs)
