"""Build private HKT1 geometry without clipping or simplifying source contours."""
import converted_files  # Install hardlink copy-on-write before asset writes.
import argparse
import hashlib
import json
import math
import shutil
import struct
from pathlib import Path
from assetripper_client import ROOT
from scene_activation import ActivationCompiler
from scene_walls import observe_walls, write_walls, identity
from scene_levers import observe_levers, write_levers
from scene_chain_platform import observe as observe_chain_platform
from scene_vine_platform import observe as observe_vine_platform

MAX_SHAPES = 256
ROOM_LANDING_TAGS = {
    # Tags observed on the Crossroads_10 source collider inventory. Triggers and
    # Battle Gate have dedicated collider handling below; Terrain is static geometry.
    'Crossroads_10': {'TransitionGate', 'Enemy Message', 'Battle Gate', 'Ghost Warrior NPC', 'Terrain'},
}

LIFT_SCENES = {'Ruins1_02', 'Ruins1_03', 'Ruins1_05b', 'Ruins1_05c', 'Ruins1_25', 'Ruins1_31',
               'Ruins2_01_b', 'Ruins2_03b', 'Ruins2_05', 'Ruins2_Watcher_Room', 'Ruins_Elevator',
               'Ruins1_05', 'Ruins1_23', 'Ruins2_03'}

# These source solids move through TownMineLift, rather than TerrainBank's
# generic lift engine. Keep their original collision flags; that controller
# updates both vertices and query bounds on every displacement.
TOWN_LIFT_SOLIDS = {
    'level7-2488.json': 'Roof Slope',
    'level7-2552.json': 'Mines Exit Lift',
    'level7-2554.json': 'Rooftop',
    'level7-2555.json': 'Roof',
    'level7-2557.json': 'Cage Closed',
}


def require(value, message):
    if not value:
        raise ValueError(message)


def lift_speed(fsm):
    """Use the authored PlayMaker variable, bounded by the runtime loader."""
    values=[v['value'] for v in fsm['variables']['floatVariables'] if v['name']=='Move Speed']
    require(len(values)==1 and type(values[0]) in (int,float) and
            math.isfinite(values[0]) and 0<values[0]<=100, 'Unsupported lift Move Speed')
    return float(values[0])


def transform_point(point, transforms):
    x,y = point
    for t in transforms:
        q,s,o = t['m_LocalRotation'],t['m_LocalScale'],t['m_LocalPosition']
        require(q['m_X']==0 and q['m_Y']==0, 'Out-of-plane transform')
        require(abs(q['m_W']**2+q['m_Z']**2-1)<1e-5, 'Invalid rotation')
        require(s['m_X']!=0 and s['m_Y']!=0, 'Singular scale')
        x,y=x*s['m_X'],y*s['m_Y']
        c, sn=1-2*q['m_Z']**2,2*q['m_W']*q['m_Z']
        x,y=c*x-sn*y+o['m_X'],sn*x+c*y+o['m_Y']
    return [x,y]


def paths(c, kind):
    o=c['m_Offset']
    if kind=='BoxCollider2D':
        x,y=c['m_Size']['m_X']/2,c['m_Size']['m_Y']/2
        result=[[[-x,-y],[x,-y],[x,y],[-x,y]]]
    elif kind=='PolygonCollider2D':
        require(len(c['m_Points']['m_Paths'])==1, 'Multiple polygon paths/holes need explicit support')
        result=[[[p['m_X'],p['m_Y']] for p in path] for path in c['m_Points']['m_Paths']]
    elif kind=='EdgeCollider2D':
        result=[[[p['m_X'],p['m_Y']] for p in c['m_Points']]]
    else:
        raise ValueError('Unsupported terrain shape: '+kind)
    return [[[x+o['m_X'],y+o['m_Y']] for x,y in path] for path in result]


def observe_lifts(mapping, read, visual_source):
    """Bind ordered RuinsLift stops through source owners and FSM references."""
    motions=[]; colliders={}; seen=set()
    if mapping['scene'] not in LIFT_SCENES:
        return motions, colliders
    visuals=json.loads((visual_source/'room-visuals-map.json').read_text())
    def visual_read(name):
        return json.loads((visual_source/name).read_text())
    def identity(name):
        return int(Path(name).stem.rsplit('-',1)[1])
    def variable(fsm, kind, name):
        values=[v['value'] for v in fsm['variables'][kind] if v['name']==name]
        require(len(values)==1, 'Missing or ambiguous lift '+name)
        return values[0]
    for row in mapping['inventory']:
        for index,group in enumerate(row['components']):
            if 'RuinsLift' not in group or group['RuinsLift'][0] in seen:
                continue
            key=group['RuinsLift'][0];seen.add(key)
            owner=row['owners'][index]
            lift=read(key)['m_Structure']; stops=lift['stopPositions']
            require(2<=len(stops)<=8 and all(type(v) in (int,float) and math.isfinite(v) and abs(v)<=100000 for v in stops)
                    and all(a>b for a,b in zip(stops,stops[1:])), 'Unsupported lift stops')
            require(len(group.get('Rigidbody2D',[]))==1, 'Missing lift body')
            body=read(group['Rigidbody2D'][0])
            require(body['m_BodyType']==1 and body['m_GravityScale']==0 and body['m_Simulated'],
                    'Lift requires a simulated kinematic body')
            fsms=[read(k)['m_Structure']['fsm'] for k in group.get('PlayMakerFSM',[])]
            fsms=[f for f in fsms if f['name']=='Lift Control']
            require(len(fsms)==1, 'Missing or ambiguous Lift Control')
            fsm=fsms[0]; initial=variable(fsm,'intVariables','Initial Position')
            require(type(initial) is int and 1<=initial<=len(stops), 'Unsupported lift Initial Position')
            position=transform_point([0,0],[read(k) for k in row['chain'][index:]])
            require(abs(position[1]-stops[initial-1])<1e-4, 'Lift initial geometry does not match stop')
            solids=[r for r in mapping['inventory'] if owner in r['owners']
                    and read(r['go'])['m_Layer']==8 and read(r['collider'])['m_Enabled']
                    and not read(r['collider'])['m_IsTrigger']]
            require(solids and all(all(read(o)['m_IsActive'] for o in r['owners']) for r in solids),
                    'Missing or inactive lift solids')
            require(all(sum('Rigidbody2D' in g for g in r['components'])==1 for r in solids),
                    'Unsupported nested lift body')
            moving=sorted({identity(r['go']) for r in visuals['inventory'] if owner in r['owners']})
            chain_ref=lift['chainsParent']
            require(chain_ref['m_FileID']==0 and chain_ref['m_PathID']!=0, 'Missing lift chains')
            chain_key=key.split('-')[0]+'-'+str(chain_ref['m_PathID'])+'.json'
            chains=[r for r in visuals['inventory'] if chain_key in r['chain']]
            chains.sort(key=lambda r: transform_point([0,0],[visual_read(t) for t in r['chain']])[1],reverse=True)
            chain_ids=[identity(r['go']) for r in chains]
            require(moving and 0<len(chain_ids)<=128 and len(set(chain_ids))==len(chain_ids),
                    'Missing or ambiguous lift art')
            controls=[None]*(len(stops)+1); positions=[None]*(len(stops)+1)
            for control in mapping['inventory']:
                for fsm_key in control['components'][0].get('PlayMakerFSM',[]):
                    lever=read(fsm_key)['m_Structure']['fsm']
                    if lever['name']=='Control Lever' and owner in control['owners'][1:]:
                        slot=0
                    elif lever['name']=='Call Lever':
                        target=variable(lever,'gameObjectVariables','Lift')
                        if target!={'m_FileID':0,'m_PathID':identity(owner)}:
                            continue
                        slot=variable(lever,'intVariables','Stop Number')
                        require(type(slot) is int and 1<=slot<=len(stops), 'Unsupported lift call stop')
                    else:
                        continue
                    c=read(control['collider'])
                    require(c['m_Enabled'] and c['m_IsTrigger'] and controls[slot] is None,
                            'Missing or ambiguous lift lever trigger')
                    contour=paths(c,control['type'])
                    require(len(contour)==1 and 3<=len(contour[0])<=16, 'Unsupported lift lever contour')
                    transforms=[read(k) for k in control['chain']]
                    controls[slot]=[transform_point(p,transforms) for p in contour[0]]
                    positions[slot]=transform_point([0,0],transforms)
            require(all(c is not None for c in controls), 'Missing lift lever')
            motion=dict(controls=controls,owners=moving,chains=chain_ids,
                        shape_ids=[identity(r['collider']) for r in solids],
                        stops=list(map(float,stops)),
                        lower=float(stops[-1]),upper=float(stops[0]),initial=float(stops[initial-1]),
                        speed=lift_speed(fsm),upper_control=positions[1],lower_control=positions[-1])
            for solid in solids:
                require(solid['collider'] not in colliders, 'Ambiguous lift solid')
                colliders[solid['collider']]=len(motions)
            motions.append(motion)
    require(motions, 'Missing source lift')
    return motions, colliders


def observe(source, region):
    require(len(region)==4 and all(math.isfinite(v) and abs(v)<=100000 for v in region) and region[0]<region[2] and region[1]<region[3], 'Invalid region')
    m=json.loads((source/'terrain-map.json').read_text())
    visual_source=source.parent/'visuals'
    tutorial_visuals = source.parent/'room-visuals'
    if m['scene'] == 'Tutorial_01' and (tutorial_visuals/'room-visuals-map.json').is_file():
        visual_source = tutorial_visuals
    visual_map_path=visual_source/'room-visuals-map.json'
    activation_maps=[m];activation_sources=[source]
    if visual_map_path.exists():
        activation_maps.append(json.loads(visual_map_path.read_text(encoding='utf-8')))
        activation_sources.append(visual_source)
    activation=ActivationCompiler(activation_sources,activation_maps)
    verified_json={}
    for name,sha in m['source_sha256'].items():
        raw=(source/name).read_bytes()
        require(hashlib.sha256(raw).hexdigest()==sha,'Changed source: '+name)
        if name.endswith('.json'):verified_json[name]=raw
    cache={}
    def read(name):
        if name not in cache:
            raw=verified_json.pop(name) if name in verified_json else (source/name).read_bytes()
            cache[name]=json.loads(raw)
        return cache[name]
    physics=read(m['physics'])
    expected={'m_DefaultContactOffset':0.01,'m_BaumgarteScale':0.2,'m_BaumgarteTimeOfImpactScale':0.75,
              'm_MaxLinearCorrection':0.2,'m_MaxTranslationSpeed':100,'m_PositionIterations':3,'m_VelocityIterations':8}
    require(all(physics.get(k)==v for k,v in expected.items()), 'Source contact settings changed; update solver observations')
    require(not physics['m_UseSubStepping'] and physics['m_AutoSyncTransforms'], 'Unsupported physics synchronization')
    require(not physics['m_QueriesStartInColliders'] and physics['m_QueriesHitTriggers'], 'Changed ray query policy')
    for layer in (9,11,26):
        require(physics['m_LayerCollisionMatrix'][layer] & (1<<8), 'Terrain layer is filtered')
    def material(name):
        p=read(name or m['default_material'])
        require(p['m_FrictionCombine']==1 and p['m_BounceCombine']==4,'Unsupported material combination')
        require(p['m_Friction']>=0 and p['m_Bounciness']==0, 'Unsupported material coefficients')
        return [p['m_Friction'],p['m_Bounciness']]
    shapes=[]; excluded=[]; lift_shapes=[]
    motion,lift_colliders=observe_lifts(m,read,visual_source)
    falling,falling_colliders=observe_chain_platform(m,read,visual_source,require)
    motion.extend(falling)
    lift_colliders.update(falling_colliders)
    vines,vine_colliders=observe_vine_platform(m,read,visual_source,require)
    lift_colliders.update({key:value+len(motion) for key,value in vine_colliders.items()})
    motion.extend(vines)
    walls,wall_colliders=observe_walls(m,read,visual_source,require)
    levers,lever_colliders=observe_levers(m,read,visual_source,transform_point,require)
    require(not (set(wall_colliders) & set(lever_colliders)), 'Terrain collider has multiple motion controllers')
    gated_region=m['scene'].startswith(('Abyss_','Fungus2_','Ruins1_','Ruins2_','Ruins_'))
    scene_slice=(m['scene'] == 'Town' or
                 m['scene'].startswith(('Crossroads_','Fungus1_','Fungus3_','Cliffs_','RestingGrounds_','Deepnest_','Hive_','Room_','Grimm_','Mines_','Waterways_')))
    for row in m['inventory']:
        c,g=read(row['collider']),read(row['go'])
        is_city_crest_blocker = m['scene']=='Fungus2_21' and row['collider']=='level186-3837.json'
        if is_city_crest_blocker:
            require(g['m_Name']=='Slam Blocker' and g['m_Layer']==8 and not c['m_IsTrigger'] and
                    'level186-1617.json' in row['owners'], 'Changed City Crest slam blocker identity')
        is_town_lift = m['scene']=='Town' and row['collider'] in TOWN_LIFT_SOLIDS
        if is_town_lift:
            require(g['m_Name']==TOWN_LIFT_SOLIDS[row['collider']] and
                    'level7-703.json' in row['owners'] and g['m_Layer']==8 and not c['m_IsTrigger'],
                    'Changed Town Mine Exit Lift solid identity')
        if scene_slice and not is_town_lift and row['collider'] not in lift_colliders and any('Rigidbody2D' in group for group in row['components']):
            excluded.append(dict(collider=row['collider'],reason='dynamic object omitted in scene slice'))
            continue
        if scene_slice and any(read(owner).get('m_Name')=='Station Door' for owner in row['owners']):
            # approximation: the station-unlock FSM opens this shutter. Generic
            # scene traversal must not bake its closed roof across the exit.
            excluded.append(dict(collider=row['collider'],reason='scripted station door kept open'))
            continue
        if ((scene_slice and (c['m_IsTrigger'] or g['m_TagString']=='Battle Gate')) or
                (gated_region and c['m_IsTrigger'] and g['m_Layer']!=25)) and identity(row['collider']) not in wall_colliders:
            # Gate/water triggers are exported separately by convert_scenes.
            # Battle gates need their room FSM; an initial closed pose would
            # permanently block the route when the combat controller is absent.
            excluded.append(dict(collider=row['collider'],reason='room trigger or battle-gate FSM omitted in scene slice'))
            continue
        if g['m_Layer'] not in (8,25):
            continue
        if scene_slice and g['m_TagString']=='Battle Gate':
            # approximation: generic scene snapshots have no battle-gate FSM.
            # Keep their passage open rather than baking an unopenable barrier.
            excluded.append(dict(collider=row['collider'],reason='scripted battle gate kept open'))
            continue
        conditional=activation.condition_for_owners(row['owners']) is not None
        dream_platform=m['scene']=='Dream_Nailcollection' and row['collider'] in {
            'level394-2538.json','level394-2519.json','level394-2526.json'}
        if dream_platform:
            require(g['m_Name']=='platform' and g['m_Layer']==8 and c['m_Enabled'] and not c['m_IsTrigger'],
                    'Changed Dream Nail reveal platform identity')
        if not c['m_Enabled'] or (not all(read(k)['m_IsActive'] for k in row['owners']) and
                                 not conditional and not is_city_crest_blocker and not dream_platform):
            excluded.append(dict(collider=row['collider'],reason='disabled/inactive'))
            continue
        ts=[read(k) for k in row['chain']]
        world_paths=[[transform_point(p,ts) for p in path] for path in paths(c,row['type'])]
        pts=[p for path in world_paths for p in path]
        box=[min(p[0] for p in pts),min(p[1] for p in pts),max(p[0] for p in pts),max(p[1] for p in pts)]
        if box[0]>region[2] or box[2]<region[0] or box[1]>region[3] or box[3]<region[1]:
            excluded.append(dict(collider=row['collider'],reason='outside selection'))
            continue
        require(c.get('m_EdgeRadius',0)==0 and not c['m_UsedByEffector'] and c['m_CompositeOperation']==0,'Unsupported edge/effector/composite')
        require(not c.get('m_AutoTiling',False),'Auto tiling requires generated geometry')
        require(not c.get('m_UseAdjacentStartPoint',False) and not c.get('m_UseAdjacentEndPoint',False),'Explicit edge adjacency needs support')
        require(all(c[k]['m_Bits']==0xffffffff for k in ('m_ForceSendLayers','m_ForceReceiveLayers','m_ContactCaptureLayers','m_CallbackLayers')), 'Unsupported contact filtering')
        require(c['m_IncludeLayers']['m_Bits']==0 and c['m_ExcludeLayers']['m_Bits']==0, 'Collider layer override')
        is_lift = row['collider'] in lift_colliders
        # Only a wall's own collider is breakable; a break floor's other Solid boxes are its
        # linked shapes, which TerrainBank requires without the breakable flag.
        collider_id = int(row['collider'].split('-')[-1].split('.')[0])
        is_wall = collider_id in wall_colliders and wall_colliders[collider_id]['collider'] == collider_id
        for group in row['components']:
            require(is_lift or is_wall or is_town_lift or 'Rigidbody2D' not in group,
                    f"Dynamic terrain requires simulation: {m['scene']}/{g['m_Name']} ({row['collider']})")
        local=row['components'][0]
        flags=int(c['m_IsTrigger']) | (2 if 'NonSlider' in local or 'Roof' in local else 0)
        if g['m_Layer']==25:
            # Hero nail terrain mask is 8|25; actor contacts and crawler rays
            # only consume layer 8. Retain these contours for nail queries.
            flags |= 32 | 128
        if is_lift:
            flags |= 512
        if is_wall:
            flags |= 1024
        if int(row['collider'].rsplit('-',1)[1].removesuffix('.json')) in lever_colliders:
            flags |= 2048
        if 'IgnoreHeroCollision' in local:
            flags |= 256
        if 'Roof' in local:
            flags |= 16
        if 'NonThunker' in local:
            flags |= 8 if any(read(k)['m_Structure']['active'] for k in local['NonThunker']) else 0
        # HeroController.OnCollisionEnter2D lands on layer 8 independently of
        # these tags. Fireball Safe changes projectile contact, not landing.
        known_scene_tag = (scene_slice or gated_region) and (g['m_TagString'] in ('Fireball Safe', 'Extra Tag', 'Terrain', 'Platform', 'Water Surface', 'Set Extrapolate') or
                           g['m_TagString'] in ROOM_LANDING_TAGS.get(m['scene'], ()))
        require(g['m_TagString'] in ('Untagged', 'Terrain', 'Battle Gate') or known_scene_tag or g['m_Layer']==25, 'Unknown terrain landing tag')
        for path in world_paths:
            require(2<=len(path)<=512 and all(math.isfinite(v) and abs(v)<=100000 for p in path for v in p),'Invalid contour')
            require(all(a!=b for a,b in zip(path,path[1:])), 'Degenerate segment')
            if row['type']!='EdgeCollider2D':
                area=sum(a[0]*b[1]-a[1]*b[0] for a,b in zip(path,path[1:]+path[:1]))
                require(abs(area)>1e-8,'Degenerate polygon')
                if area<0: path.reverse()
            shapes.append(dict(source=row['collider'],id=int(row['collider'].split('-')[-1].split('.')[0]),
                               name=g['m_Name'],kind=int(row['type']=='EdgeCollider2D'),flags=flags,
                               points=path,material=material(row['material']),
                               material_source=row['material'] or m['default_material'],
                               components=list(local)))
            if is_lift:
                lift_shapes.append(int(row['collider'].split('-')[-1].split('.')[0]))
    actor_row=next((r for r in m['inventory'] if m.get('actor_transform') in r['chain']),None)
    spawn=transform_point([0,0],[read(k) for k in actor_row['chain']]) if actor_row else [0,0]
    require(shapes and len(shapes)<=256, 'Invalid shape count')
    require(set(lift_shapes)=={s for lift in motion for s in lift['shape_ids']},
            'Selected terrain omits lift solids')
    return m,dict(build=m['build'],scene=m['scene'],region=region,crawler_spawn=spawn,
                  actor_material=material(m['actor_material']),corpse_material=material(m['corpse_material']),
                  shapes=shapes,excluded=excluded,queries_hit_triggers=True,queries_start_inside=False,
                  motion=motion,walls=walls,levers=levers)


def write_motion(report, path):
    """Write source-backed kinematic terrain controls beside the HKT1 bank."""
    motions=report.get('motion', [])
    version=6 if any(item.get('kind')==2 for item in motions) else 5 if any(item.get('kind') for item in motions) else (4 if any(len(item.get('stops',[]))>2 for item in motions) else 3)
    data=bytearray(struct.pack('<4sII', b'HKM1', version, len(motions)))
    for item in motions:
        if version>=5:
            data.extend(struct.pack('<IIIf',item.get('kind',0),*item.get('key',[0,0]),item.get('acceleration',0)))
        data.extend(struct.pack('<I9f', len(item['shape_ids']), item['lower'], item['upper'],
                                item['speed'], item['initial'], item['initial'],
                                *item['upper_control'], *item['lower_control']))
        if version>=4:
            stops=item.get('stops',[item['upper'],item['lower']])
            data.extend(struct.pack('<I',len(stops)))
            data.extend(struct.pack('<'+'f'*len(stops),*stops))
        for shape_id in item['shape_ids']: data.extend(struct.pack('<I', shape_id))
        for key in ('owners','chains'):
            data.extend(struct.pack('<I',len(item[key])))
            for owner in item[key]:data.extend(struct.pack('<I',owner))
        for control in item['controls']:
            data.extend(struct.pack('<I',len(control)))
            for point in control:data.extend(struct.pack('<2f',*point))
    path.write_bytes(data)
    return data


def convert(source, output, romfs, region):
    for p in (output,romfs):
        require(converted_files.private_output(p,ROOT,allow_build=True),'Output must remain private')
        p.mkdir(parents=True,exist_ok=True)
    m,report=observe(source,region)
    shapes=report['shapes'];n=sum(len(s['points']) for s in shapes)
    require(n<=8192,'Vertex capacity exceeded')
    data=bytearray(struct.pack('<4s4I10f',b'HKT1',len(shapes),n,1,0,*report['crawler_spawn'],*region,*report['actor_material'],*report['corpse_material']))
    first=0
    for s in shapes:
        data.extend(struct.pack('<5I2f',s['id'],s['kind'],s['flags'],first,len(s['points']),*s['material']))
        first+=len(s['points'])
    for s in shapes:
        for p in s['points']:data.extend(struct.pack('<2f',*p))
    (output/'tutorial.bin').write_bytes(data)
    shutil.copyfile(output/'tutorial.bin',romfs/'tutorial.bin')
    motion=write_motion(report, output/'terrain-motion.bin')
    shutil.copyfile(output/'terrain-motion.bin', romfs/'terrain-motion.bin')
    walls=write_walls(report['walls'],output/'terrain-walls.bin')
    shutil.copyfile(output/'terrain-walls.bin',romfs/'terrain-walls.bin')
    levers=write_levers(report['levers'],output/'terrain-levers.bin')
    shutil.copyfile(output/'terrain-levers.bin',romfs/'terrain-levers.bin')
    report['source_sha256']=m['source_sha256'];report['binary_sha256']=hashlib.sha256(data).hexdigest()
    report['motion_sha256']=hashlib.sha256(motion).hexdigest()
    report['walls_sha256']=hashlib.sha256(walls).hexdigest()
    report['levers_sha256']=hashlib.sha256(levers).hexdigest()
    (output/'manifest.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(json.dumps(dict(shapes=len(shapes),vertices=n,bytes=len(data),spawn=report['crawler_spawn'])))


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source',type=Path,default=ROOT/'_private/research/terrain')
    p.add_argument('--output',type=Path,default=ROOT/'_private/converted/terrain')
    p.add_argument('--romfs',type=Path,default=ROOT/'_private/converted/hero/romfs')
    p.add_argument('--region',type=float,nargs=4,default=[108,4,144,20],metavar=('LEFT','BOTTOM','RIGHT','TOP'))
    a=p.parse_args();require(a.region[0]<a.region[2] and a.region[1]<a.region[3], 'Invalid region')
    convert(a.source,a.output,a.romfs,a.region)
