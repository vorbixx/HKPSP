"""Append source Great Door debris and ParticleSystems to the resident decor page."""
import copy
import hashlib
import json
import math
from pathlib import Path

from PIL import Image
from convert_hero import decode_sprite
from convert_room_visuals import project


def append(data, root):
    from convert_decor_effects import limits, particle_alpha, scene_ambient
    from great_door_motion import source_data, alpha_keys
    hulls = []
    bodies = source_data(root, hulls)
    dust_alpha = []
    dust_size = []
    start_delays = []
    source = root / '_private/research/great-door-vfx-source'
    gravity_path=root/'_private/research/terrain/globalgamemanagers-15.json'
    gravity_raw=gravity_path.read_bytes()
    gravity_sha='1e4174dc9f7ea997a47bf82fe1a1929a39c9094e17530247c01a69a8fab6fd5b'
    if hashlib.sha256(gravity_raw).hexdigest()!=gravity_sha:
        raise ValueError('Changed Physics2DSettings source')
    body_gravity=json.loads(gravity_raw)['m_Gravity']['m_Y']
    mapping = json.loads((source / 'vfx-source-map.json').read_bytes())
    if mapping['build_id'] != 22529139:
        raise ValueError('Wrong Great Door source build')
    for name, sha in mapping['sha256'].items():
        if hashlib.sha256((source / name).read_bytes()).hexdigest() != sha:
            raise ValueError('Changed Great Door source: ' + name)
    def read(name):
        return json.loads((source / name).read_bytes())
    def bitmap(image, bounds):
        index = len(data['images'])
        data['images'].append(image)
        data['geometry'].append(bounds)
        return index
    def emitter(sprite, tiles=1):
        values = [0.0] * 60
        values[7:9] = [1e6, 1e6]  # FinishingRigidBody starts its timer at sleep.
        values[11:13] = [1, 1]
        # HFX integrates ParticleSystems with Physics.gravity (-9.81). Rigidbody
        # prefabs instead use the separate Physics2DSettings gravity (-60).
        values[15] = -body_gravity/9.81
        values[19] = 1e6
        values[22:38] = [1] * 16
        values[46:50] = [1] * 4
        values[59] = .2  # approximation: source polygon/box uses a swept circle.
        values[58] = .4  # approximation: contact friction for the source rigidbody material.
        return dict(sprite=sprite, tiles=tiles, count=1, rate=0, values=values,
                    cone=[0]*11, bases=[0]*12, axes=[0]*18, sorting=[0,0])
    collection = read(mapping['collections']['rock']['file'])['m_Structure']
    atlas = Image.open(source / 'resources.assets-328.png').convert('RGBA')
    first = len(data['images'])
    for name in ('rock_01', 'rock_02', 'rock_03', 'rock_04'):
        definition = collection['spriteDefinitions'][mapping['collections']['rock']['sprite_ids'][name]]
        image, bounds = decode_sprite(atlas, definition)
        bitmap(image, bounds)
    ids = []
    for owner in ('4328', '5223'):
        item = emitter(first, 4)
        config = read(mapping['objects'][owner]['parts']['DebrisParticle'][0])['m_Structure']
        item['values'][11:13] = [config['scaleMin'], config['scaleMax']]
        item['values'][20] = .5 if owner == '4328' else 0
        ids.append(len(data['emitters']));data['emitters'].append(item)
    # The door's collection contains the same two authored SpriteRenderer pieces.
    door_source = root / '_private/research/great-door-visuals'
    door_map = json.loads((door_source / 'great-door-visuals-map.json').read_bytes())
    for name, sha in door_map['source_sha256'].items():
        if hashlib.sha256((door_source/name).read_bytes()).hexdigest() != sha:
            raise ValueError('Changed door shard source: '+name)
    door_collection = json.loads((door_source/door_map['collection']).read_bytes())['m_Structure']
    door_atlas = Image.open(door_source/door_map['texture']).convert('RGBA')
    for part, name in enumerate(('additional_destruction_0002_a', 'additional_destruction_0000_a')):
        definition = next(d for d in door_collection['spriteDefinitions'] if d['name']==name)
        image, bounds = decode_sprite(door_atlas, definition)
        # Same 32 texels/unit admission as the existing door page, authored bounds.
        image = image.convert('RGBa').resize(tuple(math.ceil(x/2) for x in image.size), Image.Resampling.BOX).convert('RGBA')
        item = emitter(bitmap(image,bounds))
        item['values'][7:9] = [1e6,1e6]  # source pieces live until room unload.
        item['values'][15] *= bodies[part][10]
        item['values'][20] = bodies[part][12]
        item['values'][59] = 2*bodies[part][14]
        ids.append(len(data['emitters']));data['emitters'].append(item)
    smoke_sprite = None
    for owner, rotation in [('486',(1,0,-180)), ('500',(180,90,270)), ('465',(90,-180,0))]:
        parts = mapping['objects'][owner]['parts']
        ps = read(parts['ParticleSystem'][0])
        initial, shape = ps['m_InitialModule'], ps['m_ShapeModule']
        renderer = read(parts['ParticleSystemRenderer'][0])
        material = 'resources.assets-'+str(renderer['m_Materials'][0]['m_PathID'])+'.json'
        texture = mapping['material_media'][material]['_MainTex']
        image = Image.open(source/mapping['media'][texture]['png']).convert('RGBA')
        if owner!='465':
            dust_alpha.append(alpha_keys(ps['m_ColorModule']))
            dust_size.append([limits(ps['m_SizeModule']['m_Curve'],i/7)[curve]
                              for curve in (0,1) for i in range(8)])
            # Both emitters share the original bilinear smoke alpha texture.
            if smoke_sprite is None:
                smoke_sprite = bitmap(image,(-.5,-.5,.5,.5))
            sprite = smoke_sprite
        else:
            sprite = bitmap(image,(-.5,-.5,.5,.5))
        item=emitter(sprite)
        item['source']=parts['ParticleSystem'][0]
        delay=limits(ps['m_StartDelay_MinMaxCurve'])
        if delay[0]!=delay[1]:raise ValueError('Unsupported random door start delay')
        start_delays.append(delay[0])
        v=item['values'];v[6]=ps['m_LengthInSec']
        item['rate']=limits(ps['m_EmissionModule']['m_RateOverTime'])[1]
        item['count']=math.ceil(item['rate']*v[6]-1e-5)
        v[7:9]=limits(initial['m_StartLifetime']);v[9:11]=limits(initial['m_StartSpeed'])
        v[11:13]=limits(initial['m_StartSize']);v[13:15]=limits(initial['m_StartRotation'])
        v[15]=limits(initial['m_GravityModifier'])[1]
        clamp=ps['m_ClampVelocityModule']
        if clamp['m_Enabled']:
            v[18]=clamp['m_Dampen'];v[19]=limits(clamp['m_Magnitude'])[1]
        v[59]=0
        for offset,module,field in [(22,'m_SizeModule','m_Curve'),(38,'m_RotationModule','m_Curve')]:
            v[offset:offset+8]=[sum(limits(ps[module][field],i/7))/2 if ps[module]['m_Enabled'] else (1 if offset==22 else 0) for i in range(8)]
        v[30:38]=[particle_alpha(ps['m_ColorModule'],i/7) for i in range(8)]
        color=initial['m_StartColor']['m_MaxColor']
        tint=read(material)['m_SavedProperties']['m_Colors']['_Color']
        v[46:50]=[color['m_'+c]*tint['m_'+c] for c in 'RGBA']
        if owner!='465':
            v[46:49]=[c*a for c,a in zip(v[46:49],scene_ambient())]
        transform=copy.deepcopy(read(parts['Transform'][0]))
        # Instantiate replaces root position/rotation and retains prefab scale.
        transform['m_LocalPosition']={f'm_{a}':0 for a in 'XYZ'}
        # project() accepts quaternion; Unity Euler ZXY equals this product.
        x,y,z=[math.radians(a)/2 for a in rotation]
        sx,cx,sy,cy,sz,cz=math.sin(x),math.cos(x),math.sin(y),math.cos(y),math.sin(z),math.cos(z)
        q=(cy*sx*cz+sy*cx*sz,sy*cx*cz-cy*sx*sz,cy*cx*sz-sy*sx*cz,cy*cx*cz+sy*sx*sz)
        transform['m_LocalRotation']=dict(zip(('m_X','m_Y','m_Z','m_W'),q))
        origin=project((0,0,0),[transform])
        # Box emitters launch along local +Z, independently of box dimensions.
        forward=project((0,0,1),[transform])
        v[3]=math.atan2(forward[1]-origin[1],forward[0]-origin[0])
        for axis in range(3):
            point=[0,0,0];point[axis]=shape['m_Scale']['m_'+'XYZ'[axis]]
            p=project(point,[transform]);item['bases'][axis*2:axis*2+2]=[p[i]-origin[i] for i in range(2)]
        if owner=='500':
            # approximation: projected cone, normalized authored speed; no 3D collision.
            from convert_decor_effects import grass_cone_projection
            item['cone']=grass_cone_projection(shape,[transform]);v[5]=shape['m_RadiusParameter']['m_Value']
        item['sorting']=[renderer['m_SortingLayer'],renderer['m_SortingOrder']]
        ids.append(len(data['emitters']));data['emitters'].append(item)
    data['great_door']=dict(emitters=ids,source_sha256=mapping['sha256'],
                            bodies=bodies,hulls=hulls,dust_alpha=dust_alpha,dust_size=dust_size,start_delays=start_delays,
                            smoke_filter='source bilinear, no blur',
                            physics2d_gravity=body_gravity,physics2d_settings_sha256=gravity_sha,
                            sorting=[data['emitters'][i]['sorting'] for i in ids[4:]],
                            approximations=['Bounded polygon contacts replace native rigidbody solver ordering; small debris retains circles.',
                                            'Projected particle box/cone; eight size/spin samples, shared flat scene ambient and 32 texels/unit shards.'])
    data['pool_bound']+=1152  # all thirteen hit bursts plus both stages, even without expiry.
    # Only a few hit bursts coexist; reject unbounded capacity changes explicitly.
    data['great_door']['live_bound']=1152
