"""Derive Tutorial_01 Buzzer/stalactite numeric records (RME1) from private exports.
No original data is embedded in this tool. Polygon vertices remain private.
"""
import converted_files  # Install hardlink copy-on-write before asset writes.
import argparse
import csv
import hashlib
import json
import math
import struct
from pathlib import Path
from inspect_fsm import decode_fsm
from export_objects import require_coverage, require_lifecycle, source_lifecycle
from private_output_paths import is_private_output
from convert_tutorial import TUTORIAL_INPUTS, validate_original
ROOT = Path(__file__).resolve().parents[1]
GAME_DATA = '_private/original/Hollow Knight/hollow_knight_Data'
LANDING_INPUTS = {
    **TUTORIAL_INPUTS,
    'globalgamemanagers': '0e9a971c59c072a2ddd5bede0b64964f512bde9596c3583d23e4881bde35d7f8',
    'Managed/UnityEngine.Physics2DModule.dll': '6c863bae9264902911dc3c69dc4b354120aaa77b6e46817e838f2bf3c687a29e',
}


def fixed_step(time_settings):
    timestep = json.loads(time_settings.read_text())['m_Fixed_Timestep']
    rate = timestep['m_Rate']
    return timestep['m_Count'] * rate['m_Denominator'] / rate['m_Numerator']


def vertical_landing(capture, time_settings, spawn):
    """Use a stable native landing, never an inferred pixel displacement."""
    provenance = json.loads((capture / 'provenance.json').read_text())
    if (provenance.get('build') != '22529139' or not provenance.get('complete')
            or provenance['configuration']['target'] != 'Stalactite Hazard 5'):
        raise ValueError('Unexpected stalactite landing provenance')
    with (capture / 'probe-frames.csv').open(newline='') as stream:
        rows = {int(row['offset']): row for row in csv.DictReader(stream)}
    samples = [rows[offset] for offset in (60, 120, 180)]
    rest_y = float(samples[0]['targetY'])
    if any(not math.isfinite(float(row['targetX']))
           or not math.isfinite(float(row['targetY']))
           or abs(float(row['targetX']) - spawn[0]) > .001
           or abs(float(row['targetY']) - rest_y) > .001 for row in samples):
        raise ValueError('Native stalactite landing is not stable at its source X')
    step = fixed_step(time_settings)
    if not 0 < step <= .1 or not rest_y < spawn[1]:
        raise ValueError('Invalid native stalactite landing or fixed step')
    return rest_y, step


def reviewed_landing(original, time_settings, spawn):
    # The build 22529139 source player settled at (60.88, 45.73) at
    # frame offsets 60, 120, and 180. Keep that measured contact correction.
    validate_original(original, LANDING_INPUTS)
    step = fixed_step(time_settings)
    if (not all(math.isfinite(value) for value in spawn)
            or any(abs(actual - expected) > .001 for actual, expected in zip(spawn, (60.88, 50.87)))
            or step != 0.0199999929138322):
        raise ValueError('Stalactite Hazard 5 source spawn or fixed timestep changed')
    return 45.73, step


def convert(source, output, library):
    output = output.absolute()
    if not converted_files.private_output(output, ROOT):
        raise ValueError('Entity records must remain private')
    # Keep the worktree's shared-private alias in relative provenance paths.
    source = source.absolute()
    inventory = json.loads((source/'inventory.json').read_text())
    sources = {}
    def read(name):
        path=source/name
        raw=path.read_bytes()
        sha=hashlib.sha256(raw).hexdigest()
        if inventory['source_sha256'].get(name)!=sha:
            raise ValueError('Changed source '+name)
        sources[str(path.relative_to(ROOT))]=sha
        return json.loads(raw)
    rows=inventory['rows']
    object_rows=json.loads((source/'room-reliable-export.json').read_text())
    sources[str((source/'room-reliable-export.json').relative_to(ROOT))]=hashlib.sha256(
        (source/'room-reliable-export.json').read_bytes()).hexdigest()
    geometry=[]
    for row in rows:
        owners=[]
        for key in row['chain']:
            ref=read(key)['m_GameObject']
            if ref['m_FileID']:
                raise ValueError('External entity owner')
            owners.append(f"{row['go'].rsplit('-',1)[0]}-{ref['m_PathID']}.json")
        geometry.append(dict(go=row['go'],chain=row['chain'],owners=owners))
    object_inventory={kind:[] for kind in ('buzzers','stalactites')}
    def record_object(kind,row):
        state=source_lifecycle(dict(parts=row['parts']),geometry,object_rows,read)
        require_lifecycle(state,dict(active=True))
        object_inventory[kind].append(state)
    def name(r): return read(r['go'])['m_Name']
    def script(r,kind): return read(r['parts'][kind][0])['m_Structure']
    def transform(r): return read(r['parts']['Transform'][0])
    def xy(d): return [d['m_X'],d['m_Y']]
    def position(r):
        x=y=0
        for key in r['chain']:
            t=read(key)
            if t['m_LocalRotation']!={'m_W':1,'m_X':0,'m_Y':0,'m_Z':0}:
                raise ValueError('Rotated entity ancestor')
            x+=t['m_LocalPosition']['m_X'];y+=t['m_LocalPosition']['m_Y']
        return [x,y]
    def box(c,t=None):
        ox,oy=xy(c['m_Offset']);sx,sy=xy(c['m_Size'])
        x=y=0;scaleX=scaleY=1
        if t:
            x,y=xy(t['m_LocalPosition']);scaleX,scaleY=xy(t['m_LocalScale'])
        return [x+(ox-sx/2)*scaleX,y+(oy-sy/2)*scaleY,x+(ox+sx/2)*scaleX,y+(oy+sy/2)*scaleY]
    def child(r,type):
        tk=r['parts']['Transform'][0]
        matches=[a for a in rows if a['type']==type and len(a['chain'])>1 and a['chain'][1]==tk and name(a)=='Alert Range New']
        if len(matches)!=1: raise ValueError('Expected exactly one '+type+' child of '+name(r))
        return matches[0]
    # Validated HeroBox export shared with the existing Crawler converter.
    heroDir=ROOT/'_private/research/enemy'
    mapping=json.loads((heroDir/'crawler-map.json').read_text())
    heroKey=mapping['hero_box']['BoxCollider2D'][0]
    raw=(heroDir/heroKey).read_bytes()
    if hashlib.sha256(raw).hexdigest()!=mapping['source_sha256'][heroKey]:raise ValueError('Changed HeroBox')
    hero=box(json.loads(raw));sources[str((heroDir/heroKey).relative_to(ROOT))]=hashlib.sha256(raw).hexdigest()
    raw=library.read_bytes();lib=json.loads(raw)['m_Structure'];sources[str(library.relative_to(ROOT))]=hashlib.sha256(raw).hexdigest()
    clip=next(c for c in lib['clips'] if c['name']=='Startle')
    startle=len(clip['frames'])/clip['fps']
    settings=ROOT/'_private/research/assets/Physics2DSettings.json'
    raw=settings.read_bytes();gravity=json.loads(raw)['m_Gravity']['m_Y'];sources[str(settings.relative_to(ROOT))]=hashlib.sha256(raw).hexdigest()
    buzzers=[];stalactites=[]
    for r in rows:
        n=name(r)
        if n.startswith('Buzzer') and r['type']=='BoxCollider2D':
            c=read(r['collider']);h=script(r,'HealthManager');d=script(r,'DamageHero');recoil=script(r,'Recoil')
            fsms=[read(k)['m_Structure']['fsm'] for k in r['parts']['PlayMakerFSM']]
            fsm=decode_fsm(next(f for f in fsms if f['name']=='chaser'))
            states={s['name']:s for s in fsm['states']}
            def action(state,kind):return next(a['parameters'] for a in states[state]['actions'] if a['enabled'] and a['type'].split('.')[-1]==kind)
            def v(p,k):
                value=p[k]
                if value.get('useVariable') or 'variable' in value:raise ValueError('Variable numeric parameter '+k)
                return value['value']
            idle=action('Idle','IdleBuzz');chase=action('Chase - In Sight','ChaseObject')
            if v(chase,'targetSpread')!=0:raise ValueError('Unsupported target spread')
            ar=child(r,'CircleCollider2D');at=transform(ar);ac=read(ar['collider'])
            # Unity's CircleCollider2D radius uses the largest transform scale.
            radius=ac['m_Radius']*max(abs(at['m_LocalScale']['m_X']),abs(at['m_LocalScale']['m_Y']))
            span=next(f['value'] for f in fsm['variables']['floatVariables'] if f['name']=='Attention Span')
            stop=action('Stop','Decelerate');wait=action('Stop','Wait')
            if h['invincible'] or h['damageOverride'] or h['hasSpecialDeath']:raise ValueError('Unsupported Buzzer health')
            values=position(r)+box(c)+hero+[radius,at['m_LocalPosition']['m_Y']]+[v(idle,k) for k in ('waitMin','waitMax','speedMax','accelerationMax','roamingRange')]+[v(chase,'speedMax'),v(chase,'acceleration'),span,startle,recoil['recoilSpeedBase'],recoil['recoilDuration'],v(stop,'deceleration'),v(wait,'time')]
            ints=[h['hp'],d['damageDealt'],read(r['go'])['m_Layer'],h['smallGeoDrops']]
            buzzers.append((n,values,ints))
            record_object('buzzers',r)
        elif n.startswith('Stalactite Hazard') and r['type']=='PolygonCollider2D':
            c=read(r['collider']);s=script(r,'StalactiteControl');de=script(r,'DamageEnemies');body=read(r['parts']['Rigidbody2D'][0]);ar=child(r,'BoxCollider2D')
            paths=c['m_Points']['m_Paths']
            if len(paths)!=1 or not 3<=len(paths[0])<=16:raise ValueError('Unsupported stalactite polygon')
            vertices=[xy(p) for p in paths[0]]
            scale=xy(transform(r)['m_LocalScale']);offset=xy(c['m_Offset'])
            if scale!=[1,1]:raise ValueError('Scaled stalactite')
            vertices=[[p[0]+offset[0],p[1]+offset[1]] for p in vertices]
            embedded=next(e for e in rows if e['type']=='PolygonCollider2D' and read(e['go'])['m_Name']=='Embedded' and int(e['go'].split('-')[-1].split('.')[0])==s['embeddedVersion']['m_PathID'])
            ep=read(embedded['collider'])['m_Points']['m_Paths']
            if len(ep)!=1 or not 3<=len(ep[0])<=16:raise ValueError('Unsupported embedded polygon')
            embeddedVertices=[xy(p) for p in ep[0]]
            values=position(r)+box(read(ar['collider']),transform(ar))+hero+[s['startFallOffset'],s['fallDelay'],s['hitVelocity'],gravity*body['m_GravityScale']]+xy(transform(embedded)['m_LocalPosition'])
            ints=[read(r['go'])['m_Layer'],len(vertices),de['damageDealt'],read(embedded['go'])['m_Layer'],len(embeddedVertices)]
            stalactites.append((n,values,ints,vertices,embeddedVertices))
            record_object('stalactites',r)
    complete_objects={r['go']:r for r in rows}
    for kind, component in (('buzzers','LineOfSightDetector'),
                             ('stalactites','StalactiteControl')):
        require_coverage((r['go'] for r in complete_objects.values() if component in r['parts']),
                         (r['source_go'] for r in object_inventory[kind]),kind)
    if len(buzzers)!=3 or len(stalactites)!=8:raise ValueError('Tutorial_01 entity inventory changed')
    for method in ['StalactiteControl.cs','LineOfSightDetector.cs','AlertRange.cs','Recoil.cs','Breakable.cs','HutongGames.PlayMaker.Actions/ChaseObject.cs','HutongGames.PlayMaker.Actions/IdleBuzz.cs','HutongGames.PlayMaker.Actions/Decelerate.cs']:
        path=ROOT/'_private/research/decompiled/all'/method
        sources[str(path.relative_to(ROOT))]=hashlib.sha256(path.read_bytes()).hexdigest()
    data=bytearray(struct.pack('<4sII',b'RME1',len(buzzers),len(stalactites)))
    for _,f,u in buzzers:data+=struct.pack('<25f4I',*f,*u)
    for _,f,u,vertices,embeddedVertices in stalactites:
        data+=struct.pack('<16f5I',*f,*u)
        for p in vertices+embeddedVertices:data+=struct.pack('<2f',*p)
    # Scope this recorded trigger-step correction to the accepted owner. Other
    # owners and nail deflections keep their existing continuous contact path.
    time_settings = ROOT/'_private/research/assets/TimeManager.json'
    index = next(i for i, record in enumerate(stalactites) if record[0] == 'Stalactite Hazard 5')
    rest_y, step = reviewed_landing(ROOT / GAME_DATA, time_settings, stalactites[index][1][:2])
    sources.update({f'{GAME_DATA}/{name}': value for name, value in LANDING_INPUTS.items()})
    sources[time_settings.relative_to(ROOT).as_posix()] = hashlib.sha256(time_settings.read_bytes()).hexdigest()
    data += struct.pack('<4sII2f', b'SLP1', 1, index, rest_y, step)
    output.mkdir(parents=True,exist_ok=True)
    (output/'room-entities.bin').write_bytes(data)
    report=dict(build_id=22529139,format='RME1',object_inventory=object_inventory,buzzer_count=len(buzzers),stalactite_count=len(stalactites),source_sha256=sources,output_sha256=hashlib.sha256(data).hexdigest(),buzzers=[dict(name=n,values=f,integers=u) for n,f,u in buzzers],stalactites=[dict(name=n,values=f,integers=u,vertices=p,embedded_vertices=ep) for n,f,u,p,ep in stalactites],limitations=['Unity RNG stream is injectable, not replicated','VFX/audio/death corpse presentation is not implemented in this numeric gameplay module', 'Buzzer FSM/update/recoil scheduling is source-derived but not compared against a paced runtime trace','Polygon terrain sweep uses vertex trajectories; no Unity rotated dynamic solver equivalence'])
    report['vertical_landings'] = [dict(index=index, name=stalactites[index][0], rest_y=rest_y,
                                       native_fixed_step=step, source_build=22529139)]
    (output/'room-entities-manifest.json').write_text(json.dumps(report,indent=2))
    return report


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source',type=Path,default=ROOT/'_private/research/tutorial')
    p.add_argument('--output',type=Path,default=ROOT/'_private/converted/tutorial')
    p.add_argument('--library',type=Path,default=ROOT/'_private/research/room-entities/sharedassets6.assets-1150.json')
    a=p.parse_args();r=convert(a.source,a.output,a.library)
    print(json.dumps({k:r[k] for k in ('buzzer_count','stalactite_count','output_sha256')}))
