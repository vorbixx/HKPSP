"""Build independently resident scene packages (HKS2/HKS3)."""
import converted_files  # Install hardlink copy-on-write before asset writes.
import argparse
import hashlib
import json
import math
import re
import shutil
import struct
import tempfile
from pathlib import Path
from assetripper_client import ROOT
from convert_terrain import paths, transform_point
from convert_greenpath_enemies import write_scene_spawns
from convert_room_terrain import build as terrain, write_hkt1
from convert_room_visuals import (ADDITIVE_OWNER_NAMESPACE, additive_owner_id,
                                  convert as visuals, encode_pages)
from convert_room_resident import convert as resident
from pickup_assets import export as pickups
from scene_lighting import export_scene_lighting
from world_obstacles import export as export_world_obstacles, component_reader
from scene_remasks import observe as observe_scene_remasks, write as write_scene_remasks
from scene_enemy_rewards import extract as extract_enemy_reward, supported as supports_enemy_reward, write as write_enemy_rewards
from door_transitions import properties as door_properties, write as write_doors

from extract_scenes import SCENES as EXPORTED_SCENES
from deepnest_scenes import SCENES as DEEPNEST_SCENES
from greenpath_scenes import SCENES as GREENPATH_SCENES
from mines_waterways_scenes import SCENES as MINES_WATERWAYS_SCENES
from check_memory_budget import (_cpp_constants, old_scene_actor_paths, source_actor_banks, actor_metadata_file,
                                 actor_texture_bytes, MemoryBudgetError)
from eastern_rooms import scene_source
from scene_checkpoint import convert_cached, _remove_tree
from scene_activation import ActivationCompiler, export_scene

# Source-owned exports stay read-only; newly recovered route data belongs here.
CHECKOUT_SCENES = {'Dream_Nailcollection': 394}
SCENES = {**EXPORTED_SCENES, **GREENPATH_SCENES, **DEEPNEST_SCENES, **CHECKOUT_SCENES}
from scene_sources import saved_scenes
from convert_breakable import room_breakables
from scene_camera import export_camera_locks, authored_camera_limits
from tool_paths import TEX3DS as TEX3DS_EXE

# Goam has no killable health; boss health can live on a child collider.
PLACED_ACTORS=re.compile(r'^(?:Crawler|Climber|Fly|Buzzer|Hatcher Baby|Zombie Runner|Zombie Hornhead|Zombie Leaper|Worm|Zombie Barger|Roller|Hatcher|Mawlek Body|Giant Fly|False Knight New)(?: \d+| \(\d+\))?$')
MAX_WATER_REGIONS=32
GOAM_PREFAB=re.compile(r'^Worm(?: \d+| \(\d+\))?$')


def text(s):
    b=s.encode('ascii')
    if len(b)>=32: raise ValueError('Scene identifier too long')
    return b.ljust(32,b'\0')


STATIONS=('Room_Town_Stag_Station','Crossroads_47','Fungus1_16_alt','RestingGrounds_09')

# Stag Control stores a position independently of the PlayerData unlock flag.
# Stag Nest shares Dirtmouth's flag but has its own position and no bell.
ORIGINAL_STAG_STATIONS = (
    ('Room_Town_Stag_Station', 'openedTown', 0),
    ('Crossroads_47', 'openedCrossroads', 1),
    ('Fungus1_16_alt', 'openedGreenpath', 2),
    ('Fungus2_02', 'openedFungalWastes', 3),
    ('Fungus3_40', 'openedRoyalGardens', 4),
    ('Ruins1_29', 'openedRuins1', 5),
    ('Ruins2_08', 'openedRuins2', 6),
    ('RestingGrounds_09', 'openedRestingGrounds', 7),
    ('Deepnest_09', 'openedDeepnest', 8),
    ('Cliffs_03', 'openedTown', 9),
    ('Abyss_22', 'openedHiddenStation', 10),
)


def stag_config(source, scene):
    """Read station placement and prices from the serialized original objects/FSMs."""
    folder=source/'visuals'
    model=json.loads((folder/'room-visuals-map.json').read_text())
    rows={r['go']:r for r in model.get('mesh_renderer_audit',[]) if r.get('go')}
    cache={}
    def read(name):
        if name not in cache:cache[name]=json.loads((folder/name).read_text())
        return cache[name]
    def chain(row):
        name=row['components']['Transform'][0];result=[]
        while name:
            t=read(name);result.append(t);parent=t['m_Father']['m_PathID']
            name=name.rsplit('-',1)[0]+'-'+str(parent)+'.json' if parent else None
        return result
    def point(row):return transform_point([0,0],chain(row))
    def scale(row):return [math.prod(t['m_LocalScale'][axis] for t in chain(row)) for axis in ('m_X','m_Y')]
    def variables(row, fsm_name):
        for name in row['components'].get('PlayMakerFSM',[]):
            fsm=read(name).get('m_Structure',{}).get('fsm',{})
            if fsm.get('name')==fsm_name:
                return {v['name']:v.get('value') for values in fsm['variables'].values()
                        if isinstance(values,list) for v in values if isinstance(v,dict) and 'name' in v}
        raise ValueError('Missing '+fsm_name+' in '+scene)
    named={read(go)['m_Name']:row for go,row in rows.items()}
    if 'Stag' not in named:return None
    actor=named['Stag'];control=variables(actor,'Stag Control')
    flag=control['Station Opened Bool']
    ordered=(0,1,2,7,3,4,5,6,8,9,10)
    position=control['Station Position Number']
    matches=[entry for entry in ORIGINAL_STAG_STATIONS if entry==(scene,flag,position)]
    if not matches:raise ValueError(f'Unsupported station identity {scene} {flag} {position}')
    station=ordered.index(position)
    stand=named.get('Station Bell',named.get('Station Bell Lever'))
    if stand is None and position!=9:raise ValueError('Missing station bell in '+scene)
    bell=named.get('Bell')
    if bell is None and position!=9:raise ValueError('Missing bell in '+scene)
    key=variables(stand,'Stag Bell').get('Price String Name') if stand else None
    price=0
    if key:
        import xml.etree.ElementTree as ET
        prices={e.attrib['name']:''.join(e.itertext()) for e in ET.parse(
            ROOT/'_private/research/stag-stations/EN_Prices-decrypted.xml').iter('entry')}
        price=int(prices[key])
    return dict(station=station,price=price,position=point(actor),scale=scale(actor),
                bell=point(bell) if bell else point(actor),toll=point(stand) if stand else point(actor),bell_scale=scale(bell) if bell else [0,0],
                stand_scale=scale(stand) if stand else [0,0],arrival=point(named['Arrival Anim']),dust=point(named['Arrival Dust']),flag=flag,price_key=key)


def write_stag(target, config):
    data=struct.pack('<4sI',b'HSG2',int(config is not None))
    if config:
        data+=struct.pack('<II16f',config['station'],config['price'],*config['position'],*config['scale'],
                          *config['bell'],*config['toll'],*config['bell_scale'],*config['stand_scale'],*config['arrival'],*config['dust'])
    (target/'stag.bin').write_bytes(data)


def goam_rotation(chain):
    """Return the accumulated 2D source rotation in radians for Goam art."""
    angles=[]
    for transform in chain:
        q=transform['m_LocalRotation']
        x,y,z,w=(q['m_X'],q['m_Y'],q['m_Z'],q['m_W'])
        if not all(math.isfinite(v) for v in (x,y,z,w)) or abs(x)>1e-5 or abs(y)>1e-5:
            raise ValueError('Goam transform is not a finite 2D rotation')
        norm=math.sqrt(x*x+y*y+z*z+w*w)
        if norm==0 or abs(norm-1)>1e-4:
            raise ValueError('Goam transform has an invalid rotation quaternion')
        angles.append(2*math.atan2(z/norm,w/norm))
    angle=math.remainder(math.fsum(angles),2*math.pi)
    return 0.0 if abs(angle)<1e-6 else angle


def write_goam_visuals(target, spawns):
    rows=[(s['owner'],s['goamRotation']) for s in spawns
          if GOAM_PREFAB.fullmatch(s['prefab'])]
    data=bytearray(struct.pack('<4sI',b'HGV1',len(rows)))
    for owner,rotation in rows:
        if not math.isfinite(rotation):raise ValueError('Non-finite Goam visual rotation')
        data+=struct.pack('<If',owner,rotation)
    (target/'scene-goam-visuals.bin').write_bytes(data)


def rest_points(inventory, read, scene):
    """Use active serialized bench/bell owners; never turn ascenders into benches."""
    found={}
    for row in inventory:
        chain=[read(k) for k in row['chain']]
        for i, group in enumerate(row['components']):
            owner=row['owners'][i]
            if owner in found or not all(read(k)['m_IsActive'] for k in row['owners'][i:]):continue
            go=read(owner);name=go['m_Name'].lower()
            bench='RestBench' in group and ('bench' in name and 'ascend' not in name)
            bell=scene in STATIONS and ('bell' in name or name=='door_station') and ('PlayMakerFSM' in group)
            if not bench and not bell:continue
            # Stag stations contain several FSMs. Only their interactable bell is an anchor.
            found[owner]=dict(owner=owner,position=transform_point([0,0],chain[i:]),kind=int(not bench),
                              station=STATIONS.index(scene) if bell else 0)
    return list(found.values())


def false_knight_room(source, output, target, inventory, read, report):
    """Join Crossroads_10's primary scene to its saved level48 additive scene."""
    additive=json.loads((source/'false-knight-additive.json').read_text())
    if additive['source_scene']!='level48':raise ValueError('False Knight source must be level48')
    objects=additive['scene_objects']
    by_transform={c['id']:o for o in objects for c in o['components'] if c['type']=='Transform'}
    def named(name):
        found=[o for o in objects if o['name']==name]
        if len(found)!=1:raise ValueError('Expected one additive object: '+name)
        return found[0]
    def lineage(o):
        result=[]
        while True:
            if o in result:raise ValueError('Cyclic additive transform')
            result.append(o)
            if not o['parent']:return result
            o=by_transform[o['parent']]
    def chain(o):
        return [{key:{'m_'+axis.upper():value for axis,value in vector.items()}
                 for key,vector in parent['local_transform'].items()
                 if key in ('m_LocalPosition','m_LocalRotation','m_LocalScale')} for parent in lineage(o)]
    def box(rect):
        return [min(p[0] for p in rect),min(p[1] for p in rect),max(p[0] for p in rect),max(p[1] for p in rect)]
    def contours(o,c):
        g=c['geometry']
        if c['type']=='BoxCollider2D':
            x,y=g['m_Offset']['x'],g['m_Offset']['y'];w,h=g['m_Size']['x']/2,g['m_Size']['y']/2
            local=[[[x-w,y-h],[x+w,y-h],[x+w,y+h],[x-w,y+h]]]
        elif c['type']=='PolygonCollider2D':
            x,y=g['m_Offset']['x'],g['m_Offset']['y']
            local=[[[p['x']+x,p['y']+y] for p in path] for path in g['m_Points']['m_Paths']]
        else:raise ValueError('Unsupported additive terrain: '+c['type'])
        return [[transform_point(p,chain(o)) for p in path] for path in local]
    floor=named('FK Floor');floor_shapes=[];collapse_ids=[];collapse_owners=[]
    broken_floor_owners=[additive_owner_id(named('Broken')['id'],ADDITIVE_OWNER_NAMESPACE)]
    for o in objects:
        if floor not in lineage(o) or not all(p['active'] for p in lineage(o)):continue
        collapse=o['name']=='Break Floor' or o['name'].startswith('Floor Bit ')
        if collapse:collapse_owners.append(additive_owner_id(o['id'],ADDITIVE_OWNER_NAMESPACE))
        for c in o['components']:
            g=c.get('geometry')
            if not g or not g['m_Enabled'] or g['m_IsTrigger']:continue
            for points in contours(o,c):
                area=sum(a[0]*b[1]-a[1]*b[0] for a,b in zip(points,points[1:]+points[:1]))
                if area<0:points.reverse()
                floor_shapes.append(dict(source=f"level48-{c['id']}.json",id=c['id'],name=o['name'],kind=0,
                                         flags=0,points=points,material=report['actor_material'],
                                         components=['AdditiveTerrain']))
            if collapse:collapse_ids.append(c['id'])
    object_by_id={o['id']:o for o in objects}
    additive_visual_path=source/'visuals/level48/room-visuals-map.json'
    if not additive_visual_path.is_file():raise ValueError('Missing additive False Knight visual inventory')
    additive_visuals=json.loads(additive_visual_path.read_text())['inventory']
    for visual in additive_visuals:
        if not visual['active'] or not visual.get('sprite'):continue
        owner=int(visual['go'].rsplit('-',1)[1].split('.')[0])
        obj=object_by_id.get(owner)
        if obj is not None and obj['name']!='Broken' and floor in lineage(obj):
            collapse_owners.append(additive_owner_id(owner,ADDITIVE_OWNER_NAMESPACE))
    if not floor_shapes or not collapse_ids:raise ValueError('Missing additive False Knight floor')
    if {s['id'] for s in floor_shapes}&{s['id'] for s in report['shapes']}:
        raise ValueError('Additive collider ID collides with primary scene')
    report['shapes']+=floor_shapes
    report['additive_source']=dict(scene=additive['source_scene'],sha256=additive['source_sha256'])
    binary=write_hkt1(report,output/'terrain/geometry/tutorial.bin')
    for path in (output/'terrain/tutorial-room.bin',target/'tutorial-room.bin',target/'tutorial.bin'):
        path.write_bytes(binary)
    report['binary_sha256']=hashlib.sha256(binary).hexdigest()
    for path in (output/'terrain/manifest.json',output/'terrain/geometry/manifest.json'):
        path.write_text(json.dumps(report,indent=2))
    battle=named('Battle Scene')
    trigger=box(next(contours(battle,c)[0] for c in battle['components'] if c['type']=='BoxCollider2D'))
    gates=[]
    visual_path=source/'visuals/room-visuals-map.json'
    visual_rows=json.loads(visual_path.read_text())['inventory'] if visual_path.exists() else []
    for name in ('Battle Gate 1','Battle Gate'):
        rows=[r for r in inventory if read(r['go'])['m_Name']==name]
        if len(rows)!=1:raise ValueError('Missing False Knight arena gate: '+name)
        row=rows[0];points=[transform_point(p,[read(k) for k in row['chain']])
                           for path in paths(read(row['collider']),row['type']) for p in path]
        owners={row['go']}
        for visual in visual_rows:
            if row['go'] in visual['owners']:
                owners.update(visual['owners'][:visual['owners'].index(row['go'])+1])
        gates.append(dict(collider=int(row['collider'].split('-')[-1].split('.')[0]),bounds=box(points),
                          owners=sorted(int(owner.split('-')[-1].split('.')[0]) for owner in owners)))
    floor_top=max(p[1] for s in floor_shapes if s['name']=='Break Floor' for p in s['points'])
    arena=[gates[0]['bounds'][2],floor_top,gates[1]['bounds'][0],trigger[3]]
    room=dict(arena=arena,trigger=trigger,gates=gates,floor_colliders=sorted(set(collapse_ids)),
              floor_owners=sorted(set(collapse_owners)),
              broken_floor_owners=sorted(set(broken_floor_owners)))
    data=bytearray(struct.pack('<4s8fI',b'HKB2',*arena,*trigger,len(gates)))
    def ids(values):return struct.pack('<I',len(values))+struct.pack('<'+'I'*len(values),*values)
    for gate in gates:data+=struct.pack('<I4f',gate['collider'],*gate['bounds'])+ids(gate['owners'])
    data+=ids(room['floor_colliders'])+ids(room['floor_owners'])+ids(room['broken_floor_owners'])
    (target/'boss-room.bin').write_bytes(data)
    actor=named('False Knight New');scale=[1,1]
    for t in chain(actor):
        scale[0]*=t['m_LocalScale']['m_X'];scale[1]*=t['m_LocalScale']['m_Y']
    spawn=dict(owner=actor['id'],prefab=actor['name'],position=transform_point([0,0],chain(actor)),scale=scale,
               active=all(o['active'] for o in lineage(actor)))
    return spawn,room


def metadata(source, output, target, scene, supported_scenes=None, *, frozen_catalog=False, obstacle_sources=None):
    from scene_ambient import ambient_zone
    scene_manager_path = source/'scene-manager.json'
    scene_manager = json.loads(scene_manager_path.read_text()) if scene_manager_path.is_file() else {}
    (target/'scene-ambient.bin').write_bytes(struct.pack(
        '<4sI', b'HAP2', ambient_zone(scene_manager)))
    root=source/'terrain'
    m=json.loads((root/'terrain-map.json').read_text())
    visual_source=source/'visuals'
    visual_map=json.loads((visual_source/'room-visuals-map.json').read_text())
    activation=ActivationCompiler([root,visual_source],[m,visual_map])
    cache={}
    def read(k):
        if k not in cache:cache[k]=json.loads((root/k).read_text())
        return cache[k]
    from scene_shrooms import export as export_shrooms
    export_shrooms(m, read, target)
    report=json.loads((output/'terrain/manifest.json').read_text())
    hornet_spawn=hornet_room=None
    if scene=='Fungus1_04':
        from hornet_room import compile_room
        hornet_spawn,hornet_room=compile_room(report)
    boss_spawn,boss_room=(false_knight_room(source,output,target,m['inventory'],read,report)
                         if scene=='Crossroads_10' else (None,None))
    if scene=='Waterways_05':
        from dung_defender_room import compile_room
        boss_spawn,boss_room=compile_room(source,target,m['inventory'],read,report)
    points=[p for shape in report['shapes'] for p in shape['points']]
    bounds=[min(p[0] for p in points),min(p[1] for p in points),max(p[0] for p in points),max(p[1] for p in points)]
    limits=authored_camera_limits(source,scene)
    camera_limits_source='tk2dTileMap' if limits is not None else 'collider-fit'
    if limits is None:
        # approximation: older saved exports do not contain TileMap dimensions.
        limits=[bounds[0]+14.6,bounds[1]+8.3,bounds[2]-14.6,bounds[3]-8.3]
        for a,b in ((0,2),(1,3)):
            if limits[a]>limits[b]:limits[a]=limits[b]=(bounds[a]+bounds[b])*.5
    gates=[];spawns=[];seen=set();water=[];acid_water=[];acid_hazards=[];acid_water_owners=[]
    for row in m['inventory']:
        go=read(row['go']);chain=[read(k) for k in row['chain']]
        local=row['components'][0]
        active=all(read(k)['m_IsActive'] for k in row['owners'])
        acid_candidate=go['m_Name']=='Surface Water Region' or 'DamageHero' in local
        acid_controlled=acid_candidate and any('hasAcidArmour' in json.dumps(read(key))
                                               for key in local.get('PlayMakerFSM',[]))
        if active and go['m_Name']=='Surface Water Region':
            pts=[transform_point(p,chain) for path in paths(read(row['collider']),row['type']) for p in path]
            regions=acid_water if acid_controlled else water
            if acid_controlled:acid_water_owners.append(row['owners'])
            regions.append([min(p[0] for p in pts),min(p[1] for p in pts),max(p[0] for p in pts),max(p[1] for p in pts)])
        if active and acid_controlled:
            for key in local.get('DamageHero',[]):
                damage=read(key)['m_Structure']
                if damage['hazardType']!=3:continue
                pts=[transform_point(p,chain) for path in paths(read(row['collider']),row['type']) for p in path]
                acid_hazards.append(dict(rect=[min(p[0] for p in pts),min(p[1] for p in pts),
                                               max(p[0] for p in pts),max(p[1] for p in pts)],
                                         damage=damage['damageDealt'],type=3,owners=row['owners']))
        if 'TransitionPoint' in local:
            data=read(local['TransitionPoint'][0])['m_Structure']
            # Door Control overwrites TransitionPoint, including stale nonempty values.
            for key in local.get('PlayMakerFSM',[]):
                component=read(key)
                # A disabled component is not part of the authored runtime.  In
                # particular Crossroads_47's dormant Door Control contains a
                # stale destination while its enabled TransitionPoint is blank;
                # admitting that value would invent an exit route.
                if component.get('m_Enabled',1)==0:continue
                fsm=component['m_Structure']['fsm']
                if fsm.get('name')!='Door Control':continue
                strings={v['name']:v['value'] for v in fsm['variables']['stringVariables']}
                if strings.get('New Scene') and strings.get('Entry Gate'):
                    data=dict(data,targetScene=strings['New Scene'],entryPoint=strings['Entry Gate'])
                    break
            pts=[transform_point(p,chain) for path in paths(read(row['collider']),row['type']) for p in path]
            rect=[min(p[0] for p in pts),min(p[1] for p in pts),max(p[0] for p in pts),max(p[1] for p in pts)]
            conditional=activation.condition_for_owners(row['owners']) is not None
            gates.append(dict(name=go['m_Name'],scene=data['targetScene'],entry=data['entryPoint'],trigger=rect,
                              offset=[data['entryOffset']['x'],data['entryOffset']['y']],delay=data['entryDelay'],active=active,
                              is_door=data.get('isADoor',False),owners=row['owners'],activation_condition=conditional,
                              **door_properties(data,local,read,chain)))
        for i,group in enumerate(row['components']):
            owner=row['owners'][i]
            if owner in seen:continue
            actor=read(owner)
            if 'HealthManager' not in group and not PLACED_ACTORS.fullmatch(actor['m_Name']):continue
            seen.add(owner)
            scale=[1,1]
            for t in chain[i:]:
                scale[0]*=t['m_LocalScale']['m_X'];scale[1]*=t['m_LocalScale']['m_Y']
            spawn=dict(owner=int(owner.split('-')[-1].split('.')[0]),prefab=actor['m_Name'],
                       position=transform_point([0,0],chain[i:]),scale=scale,
                       active=all(read(k)['m_IsActive'] for k in row['owners'][i:]),owners=row['owners'][i:])
            if GOAM_PREFAB.fullmatch(actor['m_Name']):spawn['goamRotation']=goam_rotation(chain[i:])
            if supports_enemy_reward(spawn['prefab']):
                reward=extract_enemy_reward(group,read,spawn['prefab'])
                if reward is not None:spawn['reward']=reward
            spawns.append(spawn)
    gates=list({g['name']:g for g in gates}.values())
    if len(gates)>32 or any(type(g['is_door']) is not bool for g in gates):
        raise ValueError('Unsupported gate count or door flag')
    (target/'gate-inputs.bin').write_bytes(struct.pack('<4sI',b'HKG1',len(gates))+
        b''.join(struct.pack('<I',2 if g['is_door'] else 1) for g in gates))
    if boss_spawn:spawns.append(boss_spawn)
    if hornet_spawn:spawns.append(hornet_spawn)
    if len(water)>MAX_WATER_REGIONS:
        raise ValueError(f'Room has {len(water)} water regions; limit {MAX_WATER_REGIONS}')
    magic=b'HKS3' if water else b'HKS2'
    binary=bytearray(struct.pack('<4sI4f',magic,len(gates),*limits))
    scene_catalog=tuple(supported_scenes or ()) if frozen_catalog else (*SCENES,'Tutorial_01',*(supported_scenes or ()))
    discover_scenes=supported_scenes is None
    for g in gates:
        # Keep save-gated links and links across independently converted regions.
        dest=g['scene'] if g['active'] or g.get('activation_condition') else ''
        # Unity scene lookup ignores case; RomFS paths need the catalog spelling.
        if dest and discover_scenes and not any(name.casefold()==dest.casefold() for name in scene_catalog):
            scene_catalog+=tuple(saved_scenes(ROOT/'_private/research/crossroads-scenes'))
            discover_scenes=False
        dest=next((name for name in scene_catalog if name.casefold()==dest.casefold()),dest)
        binary+=struct.pack('<7f',*g['trigger'],*g['offset'],g['delay'])+text(g['name'])+text(dest)+text(g['entry'])
    binary+=struct.pack('<I',len(spawns))
    for e in spawns:
        name=e['prefab'].encode('ascii')
        if len(name)>=64:raise ValueError('Enemy prefab name too long')
        binary+=struct.pack('<I64s4fI',e['owner'],name,*e['position'],*e['scale'],int(e['active']))
    if water:
        binary+=struct.pack('<I',len(water))
        for rect in water:binary+=struct.pack('<4f',*rect)
    (target/'scene.bin').write_bytes(binary)
    obstacles=export_world_obstacles(m['inventory'],component_reader(root,obstacle_sources),target,output)
    write_doors(target/'doors.bin',gates)
    write_scene_spawns(target, spawns, bounds, hornet_room)
    write_goam_visuals(target,spawns)
    write_enemy_rewards(target/'scene-enemy-rewards.bin',spawns)
    from scene_battle import export as export_battle
    battle=export_battle(m,read,visual_map,lambda k:json.loads((visual_source/k).read_text()),spawns,target)
    remasks=observe_scene_remasks(source/'visuals')
    stag=stag_config(source,scene)
    write_stag(target,stag)
    from room_masks import compile_masks, write_masks
    masks=compile_masks(m,read,visual_map,lambda k:json.loads((visual_source/k).read_text()))
    write_masks(target/'room-masks.bin',masks)
    rests=rest_points(m['inventory'],read,scene)
    if stag:
        rests=[p for p in rests if not p['kind']]
        rests.append(dict(position=stag['toll'],kind=1,station=stag['station']))
    rest_binary=bytearray(struct.pack('<4sI',b'HKR1',len(rests)))
    for p in rests:rest_binary+=struct.pack('<2fII',*p['position'],p['kind'],p['station'])
    (target/'rest.bin').write_bytes(rest_binary)
    locks=export_camera_locks(source,target,limits)
    if len(acid_hazards)>64 or len(acid_water)>16:
        raise ValueError('Acid region count exceeds the resident room budget')
    acid_binary=bytearray(struct.pack('<4sII',b'HKA1',len(acid_hazards),len(acid_water)))
    for h in acid_hazards:acid_binary+=struct.pack('<4fII',*h['rect'],h['damage'],h['type'])
    for rect in acid_water:acid_binary+=struct.pack('<4f',*rect)
    (target/'acid.bin').write_bytes(acid_binary)
    write_scene_remasks(remasks,target/'scene-remasks.bin')
    breakables=room_breakables(source,output,target)
    manifest=dict(format=magic.decode(),scene=scene,bounds=bounds,camera_limits=limits,camera_limits_source=camera_limits_source,camera_locks=locks,gates=gates,enemy_spawns=spawns,
                  rest_points=rests,water=water,acid_water=acid_water,acid_water_owners=acid_water_owners,acid_hazards=acid_hazards,world_obstacles=obstacles,
                  breakable_count=len(breakables['records']),
                  boss_room=boss_room,battle=battle,
                  remasks=remasks,
                  masks=masks,
                  approximations=[('Camera bounds fitted to static collider extent; active serialized CameraLockArea bounds retained.'
                                   if camera_limits_source=='collider-fit' else
                                   'Authored TileMap limits and lock bounds retained; camera smoothing remains approximate.'),
                                  'Serialized initial art and terrain; room FSMs, NPCs and shops omitted. Breakable limitations are in breakables.json.',
                                  'Enemy placements retain initial active state; unsupported prefabs are reported by runtime.'])
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2))
    print(scene,'gates',[(g['name'],g['scene'],g['entry']) for g in gates],'enemies',len(spawns),flush=True)


def install_terrain(source, output, target, stage=None):
    if stage is None:
        terrain(source/'terrain',output/'terrain',target)
        return
    shutil.copytree(stage/'terrain', output/'terrain', dirs_exist_ok=True)
    # Every RomFS file the terrain stage writes (2026-10-31: terrain-walls.bin from the gated walls was
    # missing here, so all scene rules failed "converter did not produce declared outputs").
    for name in ('terrain-motion.bin', 'terrain-walls.bin', 'terrain-levers.bin', 'tutorial-room.bin', 'tutorial.bin'):
        shutil.copyfile(stage/'romfs'/name, target/name)


def convert(scene,romfs,supported_scenes=None,*,full_scale=False,texture_scale=None,output_root=None,terrain_stage=None,source_root=None, frozen_catalog=False,obstacle_sources=None):
    if texture_scale is not None:
        if texture_scale != 1:
            raise ValueError('Only texture_scale=1 is supported')
        full_scale=True
    if supported_scenes is None:
        supported_scenes=tuple(dict.fromkeys((*SCENES,*saved_scenes(ROOT/'_private/research/crossroads-scenes'))))
    source=source_root if source_root is not None else ROOT/scene_source(scene)
    output=(output_root if output_root is not None else ROOT/'_private/converted/scenes')/scene
    target=romfs/'scenes'/scene
    output.mkdir(parents=True,exist_ok=True)
    target.parent.mkdir(parents=True,exist_ok=True)
    # Keep temporary reads/writes inside this scene rule's declared outputs.
    with tempfile.TemporaryDirectory(prefix='.scene-',dir=output) as temporary:
        staging=Path(temporary)
        collectibles=target/'collectibles.bin'
        if collectibles.is_file():
            shutil.copy2(collectibles,staging/'collectibles.bin')
        if target.exists():
            _remove_tree(target,target)
        lighting=export_scene_lighting(source,staging)
        (output/'lighting.json').write_text(json.dumps(lighting,indent=2)+'\n')
        install_terrain(source,output,staging,terrain_stage)
        if frozen_catalog:
            metadata(source,output,staging,scene,supported_scenes,frozen_catalog=True,obstacle_sources=obstacle_sources)
        else:
            metadata(source,output,staging,scene,supported_scenes,obstacle_sources=obstacle_sources)
        convert_art(source,output,staging,full_scale=full_scale,actor_romfs=romfs)
        if scene == 'Fungus2_15':
            from convert_ambient_particles import compile_fungus_config
            (staging/'ambient-particles.bin').write_bytes(
                compile_fungus_config(source/'visuals/level180', 0))
        export_scene(source/'terrain',source/'visuals',output,staging)
        if scene == 'Fungus2_21':
            from scene_city_gate import export_city_gate
            export_city_gate(staging)
        staging.rename(target)


def convert_art(source,output,target,*,full_scale=False,actor_romfs=None):
    # This is the New package. Old-profile conversion remains a separate pass.
    # Keep source pixels; compact partitions may omit transparent atlas space.
    budget=_cpp_constants(ROOT/'include/render_memory.hpp')['roomTextureBudget']
    if source.name=='Fungus2_15':
        # Twelve native Mantis pages share linear RAM with this arena. Keep
        # eight RGBA4 room pages; defer optional RGBA8 fog promotion to retain
        # the existing 2 MiB runtime admission reserve with the corpse art.
        budget=min(budget,16*1024*1024)
    # Actors must share the Old allowance with room art, before a gate opens.
    # Missing/oversized optional banks must never prevent the New conversion.
    try:
        if not (output/'manifest.json').is_file():
            raise MemoryBudgetError(f'{source.name} missing room metadata')
        manifest=json.loads((output/'manifest.json').read_text())
        actor_pages=old_scene_actor_paths(source.name,manifest.get('enemy_spawns',[]),
                                         actor_romfs if actor_romfs is not None else target.parent.parent,
                                         target/'greenpath-enemies.bin')
        old_actor_bytes=sum(actor_texture_bytes(p.read_bytes()[:4]) for p in actor_pages)
    except MemoryBudgetError as error:
        print(f'Old room unavailable: {error}')
        old_actor_bytes=_cpp_constants(ROOT/'include/render_memory.hpp')['oldRoomTextureBudget']
    tex3ds=Path(TEX3DS_EXE)
    texture_jobs={}
    additive_sources=([(source/'visuals/level48','FK Floor',{'Broken'})]
                      if source.name=='Crossroads_10' else
                      [(source/'visuals/level180','Mantis Battle')]
                      if source.name=='Fungus2_15' else None)
    compact=full_scale
    fungal=output.name.startswith(('Fungus2_', 'Fungus3_'))
    tile_limits=iter((126, 62) if fungal else ())
    tile_limit=254
    while True:
        texture_jobs.clear()
        visuals(source/'visuals',output/'art',output/'staging',tex3ds,max_pages=64,
                texture_scale=1,preserve_source_dimensions=True,
                compact_tiles=compact,texture_jobs=texture_jobs,
                additive_sources=additive_sources,
                **({'compact_tile_limit': tile_limit} if fungal else {}))
        try:
            resident(output/'art',output/'resident',target,budget,tex3ds,required_texture_scale=1,
                     old_actor_texture_bytes=old_actor_bytes)
            break
        except ValueError as error:
            if not re.search(r'needs (\d+)',str(error)):
                raise
            if compact:
                next_limit=next(tile_limits,None)
                if next_limit is None:
                    raise
                tile_limit=next_limit
            compact=True
    # Compact full-scale packages use only the resident bank at runtime.
    if not full_scale:
        encode_pages(texture_jobs,tex3ds)
    else:
        for directory in (output/'art', output/'staging'):
            for stale in directory.glob('room-page-*.t3x'):
                stale.unlink()
    for name in ('room-particles.bin', 'room-breakable-fx.bin'):
        shutil.copyfile(output/'art'/name,target/name)
    manifest_path=output/'manifest.json'
    manifest=json.loads(manifest_path.read_text())
    manifest['texture_scale']=1
    manifest_path.write_text(json.dumps(manifest,indent=2)+'\n')


def main(argv=None):
    available=tuple(dict.fromkeys((*SCENES,*saved_scenes(ROOT/'_private/research/crossroads-scenes'))))
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--romfs',type=Path,default=ROOT/'_private/converted/hero/romfs')
    p.add_argument('--output-root',type=Path,default=ROOT/'_private/converted/scenes')
    p.add_argument('--terrain-stage',type=Path)
    p.add_argument('--obstacle-sources',type=Path,help='Provenance-checked recovered hazard marker dependencies')
    p.add_argument('--source',type=Path,help='Freeze the source directory for a single scene')
    p.add_argument('--full-scale',action='store_true',help='Require texture scale 1 within the resident budget')
    p.add_argument('--texture-scale',type=float,choices=(1,),dest='texture_scale',help='Compatibility alias for full-scale room textures')
    selection=p.add_mutually_exclusive_group()
    selection.add_argument('--scene',choices=available,help='Convert only this room, without shared collectibles')
    selection.add_argument('--scenes',nargs='+',choices=available,help='Freeze the regeneration graph room selection')
    selection.add_argument('--collectibles-only',action='store_true',help='Convert only the shared collectible bank')
    p.add_argument('--supported-scenes',nargs='*',choices=(*available, 'Tutorial_01'),
                   help='Freeze gate destinations independently of the rooms being converted')
    p.add_argument('--no-room-cache',action='store_true',help='Rebuild even completed room checkpoints')
    a=p.parse_args(argv)
    if a.texture_scale is not None:
        a.full_scale=True
    if a.terrain_stage is not None and a.scene is None:
        p.error('--terrain-stage requires --scene')
    if a.obstacle_sources is not None and a.scene is None:
        p.error('--obstacle-sources requires --scene')
    if a.source is not None and a.scene is None:
        p.error('--source requires --scene')
    supported_scenes=tuple(a.supported_scenes) if a.supported_scenes is not None else available
    from regen import compiler_modules
    compiler=[ROOT/name for name in sorted(compiler_modules(ROOT,('tools/convert_scenes.py',)))]
    frozen_catalog = a.source is not None and a.supported_scenes is not None
    if frozen_catalog:
        # The command already freezes the selected source and gate spellings.
        compiler = [path for path in compiler if path != ROOT/'tools/eastern_rooms.py']
    activation_module=ROOT/'tools/scene_activation.py'
    if activation_module not in compiler:compiler.append(activation_module)
    compiler.append(ROOT/'tools/scene_battle.py')
    compiler.append(Path(TEX3DS_EXE))
    compiler.append(ROOT/'include/render_memory.hpp')
    if not a.collectibles_only:
        selected=(a.scene,) if a.scene else (tuple(a.scenes) if a.scenes is not None else
                                             tuple(sorted(SCENES,key=lambda name:not name.startswith(('Room_','Grimm_')))))
        for s in selected:
            source = ROOT/a.source if a.source is not None else ROOT/scene_source(s)
            output=a.output_root/s
            target=a.romfs/'scenes'/s
            actor_inputs=sorted({p for bank in source_actor_banks(s,source)
                                 for pattern in (f'{bank}-page-*.t3x',actor_metadata_file(bank))
                                 for p in a.romfs.glob(pattern)})
            rebuilt=convert_cached(output/'conversion-checkpoint.json',
                                   [source,*compiler,*actor_inputs,*([a.terrain_stage] if a.terrain_stage else []),
                                    *([a.obstacle_sources] if a.obstacle_sources else [])],[output,target],
                                   dict(scene=s,admitted=[] if frozen_catalog else sorted(SCENES),
                                        supported_scenes=sorted(supported_scenes),full_scale=a.full_scale,
                                        obstacle_sources=str(a.obstacle_sources) if a.obstacle_sources else None),
                                   lambda:convert(s,a.romfs,supported_scenes,full_scale=a.full_scale,
                                                   output_root=a.output_root,terrain_stage=a.terrain_stage,
                                                   source_root=source,frozen_catalog=frozen_catalog,
                                                   obstacle_sources=a.obstacle_sources),force=a.no_room_cache,
                                   preserved=(target/'collectibles.bin',))
            if not rebuilt:print(f'REUSE scene {s}: source, compiler and outputs verified',flush=True)
    if a.scene is None:
        pickups(ROOT/'_private/original/Hollow Knight/hollow_knight_Data',a.romfs)


if __name__=='__main__':
    main()
