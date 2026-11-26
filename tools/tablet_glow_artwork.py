"""Source-driven tablet fades and the paired Bug/Glow tk2d room draws."""
import hashlib
import json
import struct
import math
from PIL import Image
from convert_hero import decode_sprite
from inspect_fsm import decode_fsm


def load(source):
    mapping = json.loads((source/'tablet-glow-map.json').read_text())
    if mapping['build_id'] != 22529139 or mapping['scene'] != 'Tutorial_01':
        raise ValueError('Unexpected tablet/Glow Bug build')
    for name, digest in mapping['source_sha256'].items():
        if hashlib.sha256((source/name).read_bytes()).hexdigest() != digest:
            raise ValueError('Changed tablet/Glow Bug source: '+name)
    if len(mapping['groups']) != 22 or len(mapping['tablets']) != 3:
        raise ValueError('Expected 11 paired Bug/Glow visuals and three tablets')
    read=lambda name:json.loads((source/name).read_text())
    shader = read('resources.assets-557.json')
    if shader['m_Name']!='UI/BlendModes/LinearDodge':
        raise ValueError('Glow Bug requires its source LinearDodge shader')
    passes = shader['m_ParsedForm']['m_SubShaders'][0]['m_Passes']
    blend = passes[1]['m_State']['m_RtBlend0']
    if ([p['m_Type'] for p in passes] != [2,0] or
            any(blend[k]['m_Value'] != v for k,v in
                (('m_SourceBlend',5),('m_DestinationBlend',10),
                 ('m_SourceBlendAlpha',5),('m_DestinationBlendAlpha',10)))):
        raise ValueError('Glow Bug requires per-object grab and source-alpha blending')
    material = read('sharedassets6.assets-2.json')
    if material['m_SavedProperties']['m_Colors']['_Color'] != dict(m_R=1,m_G=1,m_B=1,m_A=1):
        raise ValueError('Glow Bug source material tint needs explicit support')
    for group in mapping['groups']:
        if not read(group['parts']['GameObject'][0])['m_IsActive'] or not read(group['parts']['MeshRenderer'][0])['m_Enabled']:
            raise ValueError('Glow Bug admission requires active source renderers')
    for tablet in mapping['tablets']:
        go = read(tablet['parts']['GameObject'][0])
        components = [source/f"level6-{c['m_Component']['m_PathID']}.json" for c in go['m_Components']]
        audio = [json.loads(p.read_text()) for p in components if p.is_file()]
        audio = [a for a in audio if 'm_Resource' in a]
        if (not go['m_IsActive'] or len(audio) != 1 or
            any(audio[0][k] != v for k,v in dict(m_Enabled=1,m_Loop=True,m_PlayOnAwake=True,
                m_Pitch=1,m_Volume=0,m_Resource=dict(m_FileID=2,m_PathID=114)).items())):
            raise ValueError('Shared tablet hum requires active, same-phase native sources')
        fsm = decode_fsm(read(tablet['parts']['PlayMakerFSM'][0])['m_Structure']['fsm'])
        if any(a['enabled'] and ('Audio' in a['type'] or 'Pitch' in a['type'])
               and a['type'].split('.')[-1] not in ('FadeAudio','AudioPlayerOneShotSingle')
               for s in fsm['states'] for a in s['actions']):
            raise ValueError('Shared tablet hum cannot restart, stop or change pitch independently')
    particle_profiles(source,mapping)
    return mapping


def particle_profiles(source,mapping):
    """Admit only the six authored systems and their proven Close/Away dispatch."""
    from convert_decor_effects import limits
    from convert_room_visuals import project
    read=lambda name:json.loads((source/name).read_text())
    if len(mapping.get('particles',[]))!=6:
        raise ValueError('Re-export the six tablet particle systems')
    for tablet in mapping['tablets']:
        fsm=decode_fsm(read(tablet['parts']['PlayMakerFSM'][0])['m_Structure']['fsm'])
        states={s['name']:[a for a in s['actions'] if a['enabled']] for s in fsm['states']}
        for state,kind,count in [('Away','SetParticleEmission',1),('Close','SetParticleEmission',1),
                                 ('Close','SetParticleEmissionRate',1),('Close','PlayParticleEmitter',1)]:
            actions=[a for a in states[state] if a['type'].endswith('.'+kind)]
            if len(actions)!=count:raise ValueError('Changed tablet particle dispatch: '+state+'/'+kind)
        # The exact action targets/values are recorded and checked below.
        tablet['particle_actions']={s:[a for a in states[s] if 'Particle' in a['type']] for s in ('Init','Away','Close')}
        expected={'Init':[('SetParticleEmissionRate','Rim','emissionRate',0)],
                  'Away':[('SetParticleEmission','Glows','emission',False)],
                  'Close':[('SetParticleEmissionRate','Rim','emissionRate',500),
                           ('PlayParticleEmitter','Rim','emit',0),('SetParticleEmission','Glows','emission',True)]}
        for state,rows in expected.items():
            actual=[]
            for a in tablet['particle_actions'][state]:
                p=a['parameters'];kind=a['type'].split('.')[-1]
                key={'SetParticleEmissionRate':'emissionRate','PlayParticleEmitter':'emit','SetParticleEmission':'emission'}.get(kind)
                if not key or p[key]['useVariable'] or p.get('everyFrame',False):raise ValueError('Unsupported particle action')
                actual.append((kind,p['gameObject']['gameObject']['name'],key,p[key]['value']))
            if actual!=rows:raise ValueError('Changed tablet particle dispatch '+state)
        children={a['parameters']['storeResult']['name']:a['parameters']['childName']['value']
                  for a in states['Init'] if a['type'].endswith('.FindChild')}
        if any(children.get(n)!=n for n in ('Glows','Rim')):raise ValueError('Changed particle child binding')
    for g in mapping['particles']:
        p=read(g['parts']['ParticleSystem'][0]);r=read(g['parts']['ParticleSystemRenderer'][0])
        material=read(g['material']);shader=read(g['shader'])
        blend=shader['m_ParsedForm']['m_SubShaders'][0]['m_Passes'][0]['m_State']['m_RtBlend0']
        if (shader['m_Name']!='Legacy Shaders/Particles/Additive (Soft)' or
            [blend[k]['m_Value'] for k in ('m_SourceBlend','m_DestinationBlend')]!=[1,6] or
            material['m_SavedProperties']['m_Colors']['_TintColor']!=dict(m_R=.5,m_G=.5,m_B=.5,m_A=.5)):
            raise ValueError('Unsupported tablet particle material')
        rim=g['kind']=='rim';initial=p['m_InitialModule'];shape=p['m_ShapeModule']
        emission=p['m_EmissionModule']
        if (limits(emission['m_RateOverTime'])!=([0,0] if rim else [100,100]) or
            emission['m_BurstCount']!=0 or limits(initial['m_StartLifetime'])!=([.8,2] if rim else [1,1]) or
            sorted(limits(initial['m_StartSize']))!=[.5,1.2]):raise ValueError('Changed particle overlap budget')
        if (not read(g['parts']['GameObject'][0])['m_IsActive'] or not r['m_Enabled'] or
            p['m_Looping']!= (not rim) or p['m_PlayOnAwake']!=(not rim) or p['m_Prewarm'] or
            p['m_LengthInSec']!=.2 or p['m_MoveWithTransform']!=1 or p['m_ScalingMode']!=2 or
            p['m_SimulationSpeed']!=1 or limits(p['m_StartDelay_MinMaxCurve'])!=[.11,.11] or shape['m_Type']!=10 or
            shape['m_RadiusThickness']!=(0 if rim else 1) or shape['m_RadiusParameter']['m_Value']!=1.38 or
            limits(initial['m_StartSpeed'])!=[0,0] or limits(initial['m_GravityModifier'])!=[0,0] or
            r['m_RenderMode']!=0 or r['m_SortMode']!=0 or r['m_SortingLayer']!=0 or r['m_SortingOrder']!=0):
            raise ValueError('Unsupported tablet particle profile')
        for module in ('Velocity','ClampVelocity','Color','Rotation','Noise','Collision','UV','Sub','Trail'):
            if p['m_'+module+'Module']['m_Enabled']:raise ValueError('Unsupported tablet particle module '+module)
        if (p['m_ForceModule']['m_Enabled']!=rim or
            limits(p['m_ForceModule']['m_X'])!=[0,0] or limits(p['m_ForceModule']['m_Z'])!=[0,0] or
            (rim and (not p['m_ForceModule']['m_InWorldSpace'] or p['m_ForceModule']['m_RandomizePerFrame'])) or
            limits(initial['m_StartRotation'])!=[0,0] or initial['m_StartColor']['m_MinMaxState']!=0 or
            shape['m_Position']!=dict(m_X=0,m_Y=0,m_Z=0) or shape['m_Rotation']!=dict(m_X=0,m_Y=0,m_Z=0) or
            shape['m_Scale']!=dict(m_X=1,m_Y=1,m_Z=1) or p['m_UseUnscaledTime']):
            raise ValueError('Unsupported tablet particle force/shape/clock')
        chain=[read(n) for n in g['parts']['Transform']+g['ancestors']]
        origin=project((0,0,0),chain);x=project((1,0,0),chain);y=project((0,1,0),chain)
        if abs(x[2]-origin[2])+abs(y[2]-origin[2])>1e-6:raise ValueError('Tilted tablet particle shape')
        g.update(profile=p,origin=origin,basis=[x[0]-origin[0],x[1]-origin[1],y[0]-origin[0],y[1]-origin[1]],
                 group=0 if g['tablet']==905 else 1,start_delay=limits(p['m_StartDelay_MinMaxCurve'])[0])
    return mapping['particles']


def add_tiles(source, mapping, tiles, by_sprite):
    read = lambda name: json.loads((source/name).read_text())
    images = {}
    for group in mapping['groups']:
        library = read(group['library'])['m_Structure']['clips']
        clip = next(c for c in library if c['name'] == group['clip'])
        if clip['wrapMode'] != 0 or clip['fps'] <= 0 or len(clip['frames']) not in (4,6):
            raise ValueError('Unsupported Glow Bug animation')
        group['frames'] = []
        group['fps'] = clip['fps']
        for frame in clip['frames']:
            if frame['triggerEvent']:
                raise ValueError('Glow Bug frame event needs presentation support')
            ref = frame['spriteCollection']
            ck = group['sprite_collections'][f'{ref["m_FileID"]}:{ref["m_PathID"]}']
            key = f'glow:{ck}:{frame["spriteId"]}'
            group['frames'].append(key)
            if key in by_sprite:
                continue
            texture = group['textures'][ck]
            if texture not in images:
                images[texture] = Image.open(source/texture).convert('RGBA')
            sprite = read(ck)['m_Structure']['spriteDefinitions'][frame['spriteId']]
            bitmap, geometry = decode_sprite(images[texture],sprite)
            # Approximation: 40% resolution, premultiplied area filtering. The
            # source uses 64 texels/unit; 25.6 exceeds the closest 3DS projection
            # (~14 pixels/unit), without a ninth resident page.
            bitmap = bitmap.convert('RGBa').resize((math.ceil(bitmap.width*.4),math.ceil(bitmap.height*.4)),
                                                    Image.Resampling.BOX).convert('RGBA')
            index = len(tiles)
            by_sprite[key] = [index]
            tiles.append(dict(bitmap=bitmap,geometry=geometry,index=index,source=key))
    # Soft Additive's 2*_TintColor is white. Screen blending in the generic
    # room path computes the same S*a + D*(1-S*a), including texture alpha.
    textures={g['texture'] for g in mapping['particles']}
    if len(textures)!=1:raise ValueError('Tablet particles require one shared texture')
    key='@tablet-particle';by_sprite[key]=[len(tiles)]
    # Approximation: 32x31 premultiplied area raster; the largest source
    # billboard is under 18 LCD pixels, while 66x63 fragments a ninth page.
    particle=Image.open(source/next(iter(textures))).convert('RGBa').resize((32,31),Image.Resampling.BOX).convert('RGBA')
    tiles.append(dict(bitmap=particle,
                      geometry=(-.5,-.5,.5,.5),index=len(tiles),source=key))


def add_draws(source, mapping, draws, by_sprite, project):
    read = lambda name: json.loads((source/name).read_text())
    for group in mapping['groups']:
        chain = [read(n) for n in group['parts']['Transform']+group['ancestors']]
        origin = project((0,0,0),chain)
        px,py = project((1,0,0),chain),project((0,1,0),chain)
        sprite = read(group['parts']['tk2dSprite'][0])['m_Structure']
        scale,color = sprite['_scale'],sprite['_color']
        r = read(group['parts']['MeshRenderer'][0])
        a,c,zx = [(px[i]-origin[i])*scale['x'] for i in range(3)]
        b,d,zy = [(py[i]-origin[i])*scale['y'] for i in range(3)]
        rgba = [color[k] for k in ('r','g','b','a')]
        index = by_sprite[group['frames'][0]][0]
        owner = group['owner']
        draws.append(dict(sprite=index,owner=owner,
            values=[a,b,origin[0],c,d,origin[1],origin[2],*rgba,
                    r['m_SortingLayer'],r['m_SortingOrder'],zx,zy,5],
            sort=(r['m_SortingLayer'],r['m_SortingOrder'],-origin[2],owner,index)))
        group['origin'] = origin
    # Disjoint 12-unit tablet ranges allow at most 100 continuous Glows live
    # in each depth group. Each Rim contributes at most 100, until its tail
    # finishes (PlayParticleEmitter refuses Play while isPlaying).
    for group,count in ((0,200),(1,300)):
        z=next(g['origin'][2] for g in mapping['particles'] if g['group']==group)
        owner=0xfffffe00+group;index=by_sprite['@tablet-particle'][0]
        for _ in range(count):
            draws.append(dict(sprite=index,owner=owner,values=[0,0,0,0,0,0,z,1,1,1,0,0,0,0,0,1],
                              sort=(0,0,-z,owner,index)))


def compile_bank(source, mapping, room_source, room_mapping, by_sprite, draws, sprite_count, project):
    read = lambda name: json.loads((source/name).read_text())
    room_read = lambda name: json.loads((room_source/name).read_text())
    roots = [g for g in mapping['groups'] if g['owner'] == g['root']]
    if len(roots) != 11:
        raise ValueError('Expected 11 moving Glow Bug roots')
    data = bytearray(struct.pack('<4s5I',b'HKA1',3,11,22,sprite_count,len(draws)))
    audit = dict(tablets=[],bugs=[],visuals=[],material='LinearDodge: clamp(grab + source) before alpha blend',raster_scale=.4,
                 raster_filter='premultiplied area; source geometry and UV orientation retained')
    for tablet in mapping['tablets']:
        owner = tablet['owner']
        rows = [r for r in room_mapping['inventory'] if len(r['owners'])>1 and r['owners'][1]==f'level6-{owner}.json'
                and 'ColorFader' in r['components'] and room_read(r['go'])['m_Name'] in ('HeroLight','lit_tablet')]
        if len(rows) != 2:
            raise ValueError('Tablet needs its two active source ColorFaders')
        rows.sort(key=lambda r: room_read(r['go'])['m_Name'])
        light,lit = rows
        profiles = [room_read(r['components']['ColorFader'][0])['m_Structure'] for r in (light,lit)]
        if any(p['downColour'] != dict(r=1,g=1,b=1,a=0) or p['upDelay'] != 0 or
               not p['useInitialColour'] or p['upTime'] != 1 or p['downTime'] != 1 for p in profiles):
            raise ValueError('Unsupported tablet ColorFader profile')
        if profiles[1]['upColour'] != dict(r=1,g=1,b=1,a=1):
            raise ValueError('Unsupported lit_tablet up color')
        fsm = decode_fsm(read(tablet['parts']['PlayMakerFSM'][0])['m_Structure']['fsm'])
        states = {s['name']:s for s in fsm['states']}
        actions = lambda state,kind: next(a['parameters'] for a in states[state]['actions']
                                          if a['enabled'] and a['type'].endswith('.'+kind))
        enter = actions('Away','FloatCompare')['float2']['value']
        exit = actions('Close','FloatCompare')['float2']['value']
        up,down = actions('Close','FadeAudio'),actions('Away','FadeAudio')
        chain = [room_read(n) for n in lit['chain'][1:]]
        position = project((0,0,0),chain)
        ids = [owner,int(lit['go'].split('-')[-1].split('.')[0]),int(light['go'].split('-')[-1].split('.')[0])]
        values = [*position,enter,exit,profiles[1]['upTime'],profiles[1]['downTime'],
                  profiles[0]['upColour']['a'],up['endVolume']['value'],up['time']['value'],down['time']['value']]
        data.extend(struct.pack('<3I11f',*ids,*values))
        box = read(tablet['parts']['BoxCollider2D'][0])
        audit['tablets'].append(dict(ids=ids,values=values,inspection_collider=dict(size=box['m_Size'],offset=box['m_Offset'])))
    for group in roots:
        fsm = decode_fsm(read(group['parts']['PlayMakerFSM'][0])['m_Structure']['fsm'])
        a = next(a['parameters'] for s in fsm['states'] for a in s['actions']
                 if a['enabled'] and a['type'].endswith('.IdleBuzz'))
        settings = [a[k]['value'] for k in ('waitMin','waitMax','speedMax','accelerationMax','roamingRange')]
        values = [*group['origin'][:2],*settings]
        data.extend(struct.pack('<I7f',group['owner'],*values))
        audit['bugs'].append(dict(owner=group['owner'],values=values))
    for group in mapping['groups']:
        indices = [i for i,d in enumerate(draws) if d['owner']==group['owner']]
        if len(indices) != 1:
            raise ValueError('Glow Bug must bind one room draw per animated renderer')
        frames = [by_sprite[k][0] for k in group['frames']]
        ids = [group['owner'],group['root'],indices[0],len(frames),*(frames+[frames[-1]]*(6-len(frames)))]
        data.extend(struct.pack('<10If',*ids,group['fps']))
        audit['visuals'].append(dict(owner=group['owner'],root=group['root'],draw=indices[0],frames=frames,fps=group['fps']))
    compile_particles(source,mapping,by_sprite,draws,data,audit)
    audit['bank_sha256'] = hashlib.sha256(data).hexdigest()
    audit['source_sha256'] = mapping['source_sha256']
    return data,audit


def compile_particles(source,mapping,by_sprite,draws,data,audit):
    from convert_decor_effects import limits
    data.extend(struct.pack('<4s3I',b'HKP2',6,2,500))
    audit['particles']=[]
    for g in mapping['particles']:
        p=g['profile'];initial=p['m_InitialModule'];rim=g['kind']=='rim'
        lo,hi=limits(initial['m_StartLifetime']);size=sorted(limits(initial['m_StartSize']))
        force=sorted(limits(p['m_ForceModule']['m_Y'])) if rim else [0,0]
        color=initial['m_StartColor']['m_MaxColor']
        if any(color['m_'+k]!=1 for k in ('R','G','B')):raise ValueError('Colored tablet particle')
        profile=[*g['origin'],*g['basis'],lo,hi,*size,color['m_A'],*force,500 if rim else 100,.2,1.38,g['start_delay']]
        # Exact Hermite keys, up to four per side; avoid sampled-curve error.
        c=p['m_SizeModule']['m_Curve']
        keys=[]
        for side in ('m_MinCurve','m_MaxCurve'):
            rows=c[side]['m_Curve'] if c['m_MinMaxState']==2 or side=='m_MaxCurve' else c['m_MaxCurve']['m_Curve']
            if not 2<=len(rows)<=4:raise ValueError('Unsupported size curve')
            keys.extend(v for row in rows+[rows[-1]]*(4-len(rows)) for v in
                        (row['m_Time'],row['m_Value'],row['m_InSlope'],row['m_OutSlope']))
        ids=[g['owner'],next(i for i,t in enumerate(mapping['tablets']) if t['owner']==g['tablet']),int(rim),g['group']]
        data.extend(struct.pack('<4I50f',*ids,*profile,*keys))
        audit['particles'].append({k:g[k] for k in ('owner','tablet','kind','material','shader','texture','origin','basis','group','start_delay')})
    for group,count in ((0,200),(1,300)):
        indices=[i for i,d in enumerate(draws) if d['owner']==0xfffffe00+group]
        if len(indices)!=count or indices!=list(range(indices[0],indices[0]+count)):raise ValueError('Particle reservation order')
        data.extend(struct.pack('<3I',indices[0],count,by_sprite['@tablet-particle'][0]))
    audit['particle_dispatch']={t['owner']:t['particle_actions'] for t in mapping['tablets']}
    audit['particle_capacity']=500
    audit['particle_cost']=dict(extra_linear_bytes=0,resident_pages=8,draw_slots=500,
                                ordinary_state_and_metadata_cap=192*1024,hardware_timing='unmeasured')
    audit['particle_clock']='Scaled fixed .02s; fractional birth offsets; PresentationPosition interpolation; reset clears all particles.'
    audit['particle_activation']='Init sets Rim rate 0; Away disables Glows; Close sets Rim 500/s and Play(emit=0), enables Glows. Rim Play is guarded by !isPlaying.'
    audit['particle_approximations']=['Seeded independent RNG (Unity RNG identity is not reproduced).',
                                    'Analytic constant acceleration; no Unity integration reconstruction.',
                                    '32x31 premultiplied area-filtered raster from the source 66x63 texture.',
                                    'Soft intersection fade is omitted: the room path has no scene depth texture.']
