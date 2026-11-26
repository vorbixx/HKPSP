"""Package the researched Tutorial_01 manifest as the runtime HKQ1 bank.

Room coordinates stay private. Diagnostic terrain and encounter files are kept.
"""
import converted_files  # Install hardlink copy-on-write before asset writes.
import hashlib
import json
import struct
from pathlib import Path
from assetripper_client import ROOT
from convert_terrain import transform_point,paths
from inspect_fsm import decode_fsm
from export_objects import require_coverage, validate_tutorial_objects
from enemy_messages import load_messages


def fade_seconds(fsm):
    fade=next(s for s in fsm['states'] if s['name']=='Fade')['actions']
    assert len(fade)==1 and fade[0]['enabled'] and fade[0]['type'].endswith('.iTweenFadeTo')
    p=fade[0]['parameters']
    assert p['gameObject']['ownerOption']==0 and p['alpha']['value']==0 and p['includeChildren']['value']
    assert p['easeType']==21 and p['loopType']==0 and p['delay']['value']==0
    assert p['time']['value']>0 and p['namedValueColor']['value']=='_Color' and not p['realTime']['value']
    return p['time']['value']


def player_reveals(visuals,inventory):
    """The simple player-enter -> HIT fade, not the distinct UNCOVER FSMs."""
    read=lambda name:json.loads((visuals/name).read_text(encoding='utf-8'))
    reveals=[];seen=set()
    for row in inventory:
        for name in row['components'].get('PlayMakerFSM',[]):
            if name in seen:continue
            seen.add(name)
            fsm=decode_fsm(read(name)['m_Structure']['fsm'])
            if fsm['name']!='unmasker':continue
            idle=next(s for s in fsm['states'] if s['name']==fsm['start'])
            if not any(t['fsmEvent']['name']=='HIT' and t['toState']=='Fade' for t in idle['transitions']):continue
            colliders=[(n,read(n)) for names in row['components'].values() for n in names]
            colliders=[(n,c) for n,c in colliders if c.get('m_Enabled') and c.get('m_IsTrigger')]
            if not colliders:continue # Door receivers are driven by Breakable.HIT.
            actions=[a for a in idle['actions'] if a['enabled']]
            assert len(actions)==1 and actions[0]['type'].endswith('.Trigger2dEvent')
            p=actions[0]['parameters']
            assert p['trigger']==0 and p['collideTag']['value']=='Player' and p['sendEvent']=='HIT'
            transforms=[read(n) for n in row['chain']];rects=[]
            for _,collider in colliders:
                assert 'm_Size' in collider
                points=[transform_point(p,transforms) for path in paths(collider,'BoxCollider2D') for p in path]
                # This compact runtime contract supports axis-aligned source triggers.
                xs=sorted({round(p[0],5) for p in points});ys=sorted({round(p[1],5) for p in points})
                assert len(xs)==2 and len(ys)==2
                rects.append([min(p[0] for p in points),min(p[1] for p in points),max(p[0] for p in points),max(p[1] for p in points)])
            owners=sorted({int(r['go'].split('-')[-1].split('.')[0]) for r in inventory if row['go'] in r['owners']})
            reveals.append(dict(owner=int(row['go'].split('-')[-1].split('.')[0]),triggers=rects,owners=owners,
                                seconds=fade_seconds(fsm),fsm=name,colliders=[n for n,_ in colliders]))
    return reveals


def player_remasks(visuals,inventory):
    """Tut_msk_04: reversible material fade, opaque outside its trigger."""
    read=lambda name:json.loads((visuals/name).read_text(encoding='utf-8'))
    row=next(r for r in inventory if r['go']=='level6-662.json')
    name='level6-12056.json'
    assert name in row['components']['PlayMakerFSM']
    fsm=decode_fsm(read(name)['m_Structure']['fsm'])
    assert fsm['name']=='remasker' and fsm['start']=='Pause'
    pause=[a['parameters'] for a in next(s for s in fsm['states'] if s['name']=='Pause')['actions']
           if a['enabled'] and a['type'].endswith('.Wait')]
    assert len(pause)==1 and pause[0]['time']['value']==1 and not pause[0]['realTime']
    seconds=next(v['value'] for v in fsm['variables']['floatVariables'] if v['name']=='Fade Time')
    assert seconds>0
    for state_name,alpha,event,trigger,destination in [('Idle',None,'UNCOVER',1,'Fade Out'),
            ('Fade Out',0,'COVER',2,'Fade In'),('Fade In',1,'UNCOVER',1,'Fade Out')]:
        state=next(s for s in fsm['states'] if s['name']==state_name)
        assert any(t['fsmEvent']['name']==event and t['toState']==destination for t in state['transitions'])
        actions=[a for a in state['actions'] if a['enabled']]
        action=next(a['parameters'] for a in actions if a['type'].endswith('.Trigger2dEvent'))
        assert action['trigger']==trigger and action['collideTag']['value']=='Player' and action['sendEvent']==event
        if alpha is not None:
            fade=next(a['parameters'] for a in actions if a['type'].endswith('.iTweenFadeTo'))
            assert fade['gameObject']['ownerOption']==0 and fade['alpha']['value']==alpha
            assert fade['includeChildren']['value'] and fade['easeType']==21 and fade['time']['variable']=='Fade Time'
            assert fade['loopType']==0 and fade['delay']['value']==0 and fade['stopOnExit']['value']
    collider_name='level6-8043.json';collider=read(collider_name)
    assert collider['m_Enabled'] and collider['m_IsTrigger']
    transforms=[read(n) for n in row['chain']]
    polygons=paths(collider,'PolygonCollider2D')
    assert len(polygons)==1 and len(polygons[0])==7
    points=[transform_point(p,transforms) for p in polygons[0]]
    rect=[min(p[0] for p in points),min(p[1] for p in points),max(p[0] for p in points),max(p[1] for p in points)]
    owners=sorted({int(r['go'].split('-')[-1].split('.')[0]) for r in inventory if row['go'] in r['owners']})
    inputs=[name,collider_name,*row['chain']]
    return [dict(trigger=rect,polygon=points,owners=owners,seconds=seconds,opaque_inside=False,source=name,
        startup_pause=pause[0]['time']['value'],startup_hero_positioned=True,
        source_sha256={n:hashlib.sha256((visuals/n).read_bytes()).hexdigest() for n in inputs})]


def fury_remasks(visuals,inventory,source=None):
    """Fury room cover: fade all children out while Player stays inside."""
    source=source or ROOT/'_private/research/fury-mask'
    manifest=json.loads((source/'manifest.json').read_text())
    assert manifest['build']==22529139
    for filename,digest in manifest['source_sha256'].items():
        if hashlib.sha256((source/filename).read_bytes()).hexdigest()!=digest:
            raise ValueError('Changed Fury mask source: '+filename)
    read=lambda name:json.loads(((source if name in manifest['source_sha256'] else visuals)/name).read_text(encoding='utf-8'))
    name='level6-12037.json';collider_name='level6-8117.json'
    fsm=decode_fsm(read(name)['m_Structure']['fsm'])
    assert fsm['name']=='unmasker' and fsm['start']=='Pause'
    seconds=next(v['value'] for v in fsm['variables']['floatVariables'] if v['name']=='Fade Time')
    assert seconds>0
    for state_name,alpha,event,trigger,destination in [('Idle',None,'UNCOVER',1,'Fade Out'),
            ('Fade Out',0,'COVER',2,'Fade In'),('Fade In',1,'UNCOVER',1,'Fade Out')]:
        state=next(s for s in fsm['states'] if s['name']==state_name)
        assert any(t['fsmEvent']['name']==event and t['toState']==destination for t in state['transitions'])
        actions=[a for a in state['actions'] if a['enabled']]
        action=next(a['parameters'] for a in actions if a['type'].endswith('.Trigger2dEvent'))
        assert action['trigger']==trigger and action['collideTag']['value']=='Player' and action['sendEvent']==event
        if alpha is not None:
            fade=next(a['parameters'] for a in actions if a['type'].endswith('.iTweenFadeTo'))
            assert fade['gameObject']['ownerOption']==0 and fade['alpha']['value']==alpha
            assert fade['includeChildren']['value'] and fade['easeType']==21 and fade['time']['variable']=='Fade Time'
            assert fade['loopType']==0 and fade['delay']['value']==0 and fade['stopOnExit']['value'] and not fade['realTime']['value']
    row=next(r for r in inventory if r['go']=='level6-248.json')
    chain=row['chain'][1:]
    assert read(chain[0])['m_GameObject']['m_PathID']==210
    collider=read(collider_name);assert collider['m_Enabled'] and collider['m_IsTrigger']
    points=[transform_point(p,[read(n) for n in chain]) for path in paths(collider,'BoxCollider2D') for p in path]
    assert len({round(p[0],5) for p in points})==2 and len({round(p[1],5) for p in points})==2
    rect=[min(p[0] for p in points),min(p[1] for p in points),max(p[0] for p in points),max(p[1] for p in points)]
    owners=sorted({int(r['go'].split('-')[-1].split('.')[0]) for r in inventory if 'level6-210.json' in r['owners']})
    assert 248 in owners
    inputs=[name,collider_name,*chain]
    return [dict(trigger=rect,owners=owners,seconds=seconds,opaque_inside=False,source=name,
        source_sha256={n:hashlib.sha256(((source if n in manifest['source_sha256'] else visuals)/n).read_bytes()).hexdigest() for n in inputs})]


def inverse_remasks(visuals,inventory):
    """Observed right remask: transparent outside, opaque inside, reversible fade.

    The x135 original-player probe verified both children through enter/exit.
    Other UNCOVER automata have different rules and are deliberately excluded.
    """
    read=lambda name:json.loads((visuals/name).read_text(encoding='utf-8'))
    fsm=decode_fsm(read('level6-12038.json')['m_Structure']['fsm'])
    assert fsm['name']=='unmasker' and fsm['start']=='Pause'
    seconds=next(v['value'] for v in fsm['variables']['floatVariables'] if v['name']=='Fade Time')
    assert seconds>0
    for name,alpha,event,trigger,destination in [('Idle',0,'UNCOVER',1,'Fade Out'),
            ('Fade Out',1,'COVER',2,'Fade In'),('Fade In',0,'UNCOVER',1,'Fade Out')]:
        state=next(s for s in fsm['states'] if s['name']==name)
        assert any(t['fsmEvent']['name']==event and t['toState']==destination for t in state['transitions'])
        actions=[a for a in state['actions'] if a['enabled']]
        fade=next(a['parameters'] for a in actions if a['type'].endswith('.iTweenFadeTo') and a['parameters']['gameObject']['ownerOption']==0)
        assert fade['alpha']['value']==alpha and fade['includeChildren']['value'] and fade['easeType']==21
        assert fade['loopType']==0 and fade['delay']['value']==0 and fade['stopOnExit']['value']
        assert name=='Idle' or fade['time']['variable']=='Fade Time'
        action=next(a['parameters'] for a in actions if a['type'].endswith('.Trigger2dEvent'))
        assert action['trigger']==trigger and action['collideTag']['value']=='Player' and action['sendEvent']==event
    row=next(r for r in inventory if r['go']=='level6-2128.json')
    transforms=[read(n) for n in row['chain'][1:]]
    assert transforms[0]['m_GameObject']['m_PathID']==245
    # FindChild("Inverse Mask") has no target in this instance.
    for child in transforms[0]['m_Children']:
        tr=read(f"level6-{child['m_PathID']}.json")
        assert read(f"level6-{tr['m_GameObject']['m_PathID']}.json")['m_Name']!='Inverse Mask'
    collider=read('level6-8119.json');assert collider['m_Enabled'] and collider['m_IsTrigger']
    points=[transform_point(p,transforms) for path in paths(collider,'BoxCollider2D') for p in path]
    assert len({round(p[0],5) for p in points})==2 and len({round(p[1],5) for p in points})==2
    rect=[min(p[0] for p in points),min(p[1] for p in points),max(p[0] for p in points),max(p[1] for p in points)]
    owners=sorted({int(r['go'].split('-')[-1].split('.')[0]) for r in inventory if 'level6-245.json' in r['owners']})
    inputs=['level6-12038.json','level6-8119.json',*row['chain'][1:]]
    return [dict(trigger=rect,owners=owners,seconds=seconds,source='level6-12038.json',
        startup_pause=2,startup_fade=.01,startup_hero_positioned=True,
        source_sha256={name:hashlib.sha256((visuals/name).read_bytes()).hexdigest() for name in inputs})]


def small_secret_remask(visuals,inventory):
    """The visible msk_generic cover; saved Activated is never tested by its FSM."""
    read=lambda name:json.loads((visuals/name).read_text(encoding='utf-8'))
    row=next(r for r in inventory if r['go']=='level6-2857.json')
    name='level6-12231.json';fsm=decode_fsm(read(name)['m_Structure']['fsm'])
    assert fsm['name']=='remasker_inverse' and fsm['start']=='Pause'
    states={s['name']:s for s in fsm['states']}
    assert not any(a['enabled'] and a['type'].endswith('.BoolTest') for s in fsm['states'] for a in s['actions'])
    seconds=next(v['value'] for v in fsm['variables']['floatVariables'] if v['name']=='Fade Time')
    assert seconds==.5
    for state,alpha,event,trigger,destination in [('Idle',None,'UNCOVER',1,'Fade Out'),
            ('Fade Out',0,'COVER',2,'Fade In'),('Fade In',1,'UNCOVER',1,'Fade Out')]:
        actions=[a for a in states[state]['actions'] if a['enabled']]
        assert any(t['fsmEvent']['name']==event and t['toState']==destination for t in states[state]['transitions'])
        p=next(a['parameters'] for a in actions if a['type'].endswith('.Trigger2dEvent'))
        assert p['trigger']==trigger and p['sendEvent']==event and p['collideTag']['value']=='Player'
        if alpha is not None:
            p=next(a['parameters'] for a in actions if a['type'].endswith('.iTweenFadeTo'))
            assert p['alpha']['value']==alpha and p['time']['variable']=='Fade Time' and p['easeType']==21
            assert p['gameObject']['ownerOption']==0 and p['includeChildren']['value'] and p['stopOnExit']['value']
    pause=[a for a in states['Pause']['actions'] if a['enabled']]
    wait=next(a['parameters'] for a in pause if a['type'].endswith('.Wait'))
    positioned=next(a['parameters'] for a in pause if a['type'].endswith('WaitForHeroInPosition'))
    assert wait['time']['value']==1 and wait['finishEvent']==positioned['sendEvent']=='FINISHED'
    renderer=read(row['renderer']);assert renderer['m_Enabled'] and renderer['m_Sprite']['m_PathID']!=0
    transforms=[read(n) for n in row['chain']];assert not transforms[0]['m_Children']
    collider='level6-8289.json';shape=read(collider);assert shape['m_Enabled'] and shape['m_IsTrigger']
    points=[transform_point(p,transforms) for path in paths(shape,'BoxCollider2D') for p in path]
    rect=[min(p[0] for p in points),min(p[1] for p in points),max(p[0] for p in points),max(p[1] for p in points)]
    inputs=[name,collider,row['renderer'],*row['chain'],'level6-12864.json']
    return [dict(trigger=rect,owners=[2857],seconds=seconds,opaque_inside=False,startup_pause=1,
        startup_hero_positioned=True,source=name,
        source_sha256={n:hashlib.sha256((visuals/n).read_bytes()).hexdigest() for n in inputs})]


def secret_regions(visuals,inventory):
    """Persistent sound discoveries. Their null sprites cannot cover the room."""
    read=lambda name:json.loads((visuals/name).read_text(encoding='utf-8'))
    result=[]
    for owner,fsm_id,collider_id,persistent_id,flag in [(2002,12146,8227,12858,2048),(3005,12238,8298,12865,4096)]:
        row=next(r for r in inventory if r['go']==f'level6-{owner}.json')
        name=f'level6-{fsm_id}.json';fsm=decode_fsm(read(name)['m_Structure']['fsm'])
        states={s['name']:s for s in fsm['states']}
        assert fsm['name']=='unmasker' and fsm['start']=='Pause'
        renderer=read(row['renderer']);assert renderer['m_Sprite']['m_PathID']==0
        transforms=[read(n) for n in row['chain']];assert not transforms[0]['m_Children']
        persistent=read(f'level6-{persistent_id}.json')['m_Structure']
        assert not persistent['semiPersistent'] and not persistent['dontSave']
        trigger=next(a['parameters'] for a in states['Idle Stay']['actions'] if a['enabled'] and a['type'].endswith('.Trigger2dEvent'))
        assert trigger['trigger']==1 and trigger['sendEvent']=='UNCOVER' and trigger['collideTag']['value']=='Player'
        check=next(a['parameters'] for a in states['Idle']['actions'] if a['enabled'] and a['type'].endswith('.BoolTest'))
        assert check['boolVariable']['variable']=='Activated' and check['isTrue']=='ACTIVATE'
        for state,seconds in [('Fade',.5),('Activate',.1)]:
            fade=next(a['parameters'] for a in states[state]['actions'] if a['enabled'] and a['type'].endswith('.iTweenFadeTo'))
            assert abs(fade['time']['value']-seconds)<1e-7 and fade['alpha']['value']==0
        sound=next(a['parameters'] for a in states['Sound']['actions'] if a['enabled'])
        assert sound['audioClip']['value']=={'m_FileID':2,'m_PathID':157}
        assert sound['pitchMin']['value']==sound['pitchMax']['value']==sound['volume']['value']==1
        collider=f'level6-{collider_id}.json';shape=read(collider);assert shape['m_Enabled'] and shape['m_IsTrigger']
        points=[transform_point(p,transforms) for path in paths(shape,'BoxCollider2D') for p in path]
        rect=[min(p[0] for p in points),min(p[1] for p in points),max(p[0] for p in points),max(p[1] for p in points)]
        inputs=[name,collider,row['renderer'],*row['chain'],f'level6-{persistent_id}.json']
        result.append(dict(owner=owner,trigger=rect,save_flag=flag,source=name,
            persistent_id=read(row['go'])['m_Name'],reload_fade=.1,discovery_fade=.5,
            source_sha256={n:hashlib.sha256((visuals/n).read_bytes()).hexdigest() for n in inputs}))
    return result


def startup_inverse_remasks(visuals,inventory):
    """Pause/Idle/enter/exit remasker_inverse instances, including startup events."""
    read=lambda name:json.loads((visuals/name).read_text(encoding='utf-8'))
    masks=[]
    for row in inventory:
        for fsm_name in row['components'].get('PlayMakerFSM',[]):
            if fsm_name not in ('level6-12105.json','level6-12182.json'):continue
            fsm=decode_fsm(read(fsm_name)['m_Structure']['fsm'])
            if fsm['name']!='remasker_inverse':continue
            assert fsm['start']=='Pause'
            expected_go={'level6-12105.json':'level6-1370.json',
                         'level6-12182.json':'level6-2344.json'}[fsm_name]
            assert row['go']==expected_go
            states={s['name']:s for s in fsm['states']}
            pause=[a for a in states['Pause']['actions'] if a['enabled']]
            wait=next(a['parameters'] for a in pause if a['type'].endswith('.Wait'))
            positioned=next(a['parameters'] for a in pause if a['type'].endswith('WaitForHeroInPosition'))
            assert wait['finishEvent']==positioned['sendEvent']=='FINISHED'
            assert wait['time']['value']==2 and positioned['skipIfAlreadyPositioned']['value'] is False
            assert any(t['fsmEvent']['name']=='FINISHED' and t['toState']=='Idle' for t in states['Pause']['transitions'])
            seconds=next(v['value'] for v in fsm['variables']['floatVariables'] if v['name']=='Fade Time')
            startup_fade=None
            for state_name,alpha,event,trigger,destination in [('Idle',0,'UNCOVER',1,'Fade Out'),
                    ('Fade Out',1,'COVER',2,'Fade In'),('Fade In',0,'UNCOVER',1,'Fade Out')]:
                state=states[state_name];actions=[a for a in state['actions'] if a['enabled']]
                assert any(t['fsmEvent']['name']==event and t['toState']==destination for t in state['transitions'])
                fade=next(a['parameters'] for a in actions if a['type'].endswith('.iTweenFadeTo') and a['parameters']['gameObject']['ownerOption']==0)
                assert fade['alpha']['value']==alpha and fade['includeChildren']['value'] and fade['easeType']==21
                assert fade['loopType']==0 and fade['delay']['value']==0 and fade['stopOnExit']['value']
                if state_name=='Idle':startup_fade=fade['time']['value']
                else:assert fade['time']['variable']=='Fade Time'
                action=next(a['parameters'] for a in actions if a['type'].endswith('.Trigger2dEvent'))
                assert action['trigger']==trigger and action['collideTag']['value']=='Player' and action['sendEvent']==event
            transforms=[read(n) for n in row['chain']]
            # Neither authored instance has the optional Inverse Mask child.
            assert not transforms[0]['m_Children']
            names=[f"level6-{c['m_Component']['m_PathID']}.json" for c in read(row['go'])['m_Components']]
            colliders=[(n,read(n)) for n in names]
            colliders=[c for c in colliders if c[1].get('m_Enabled') and c[1].get('m_IsTrigger')]
            assert len(colliders)==1
            collider_name,collider=colliders[0]
            kind='PolygonCollider2D' if 'm_Points' in collider else 'BoxCollider2D'
            polygons=paths(collider,kind);assert len(polygons)==1
            points=[transform_point(p,transforms) for p in polygons[0]]
            rect=[min(p[0] for p in points),min(p[1] for p in points),max(p[0] for p in points),max(p[1] for p in points)]
            owners=sorted({int(r['go'].split('-')[-1].split('.')[0]) for r in inventory if row['go'] in r['owners']})
            assert owners and seconds==.5 and wait['time']['value']>0 and abs(startup_fade-.01)<1e-7
            assert expected_go in {f'level6-{owner}.json' for owner in owners}
            inputs=[fsm_name,collider_name,*row['chain']]
            masks.append(dict(trigger=rect,polygon=points if kind=='PolygonCollider2D' else [],owners=owners,
                seconds=seconds,opaque_inside=True,startup_pause=wait['time']['value'],startup_fade=startup_fade,
                startup_hero_positioned=True,source=fsm_name,
                source_sha256={n:hashlib.sha256((visuals/n).read_bytes()).hexdigest() for n in inputs}))
    assert len(masks)==2
    return masks


def door_artwork(visuals,inventory,owner):
    """Only the researched Breakable -> HIT -> linear child fade branch."""
    read=lambda name:json.loads((visuals/name).read_text(encoding='utf-8'))
    row=next(r for r in inventory if r['go']==f'level6-{owner}.json')
    scripts=[read(k).get('m_Structure',{}) for names in row['components'].values() for k in names]
    source=next(s for s in scripts if 'hitEventReciever' in s)
    roots={f"level6-{p['m_PathID']}.json" for p in source['wholeParts']}
    ids=lambda roots:sorted({int(r['go'].split('-')[-1].split('.')[0]) for r in inventory if roots.intersection(r['owners'])})
    whole=ids(roots);target=source['hitEventReciever']['m_PathID']
    if not target:return whole,[],0
    root=f'level6-{target}.json';receiver=next(r for r in inventory if r['go']==root)
    fsm=decode_fsm(read(receiver['components']['PlayMakerFSM'][0])['m_Structure']['fsm'])
    idle=next(s for s in fsm['states'] if s['name']==fsm['start'])
    assert any(t['fsmEvent']['name']=='HIT' and t['toState']=='Fade' for t in idle['transitions'])
    return whole,ids({root}),fade_seconds(fsm)


def convert():
    source=ROOT/'_private/converted/tutorial/manifest.json'
    m=json.loads(source.read_text())
    research=ROOT/'_private/research/tutorial'
    read_source=lambda name:json.loads((research/name).read_text())
    for name, expected in m['source_sha256'].items():
        if hashlib.sha256((research/name).read_bytes()).hexdigest()!=expected:
            raise ValueError('Changed tutorial source '+name+'; regenerate the manifest')
    inventory=validate_tutorial_objects(m,read_source('room-reliable-export.json'),
        read_source('collider-geometry.json'),read_source,read_source('inventory.json')['rows'])
    # Preserve existing actor indices; dormant actors follow the active inventory.
    crawlers=sorted(m['crawlers'],key=lambda c:not c['active'])
    # HKQ1 has no source IDs. RoomConfig's source-depth metadata uses this
    # frozen order (original player probe, first activation), never audio-ID order.
    assert [c['source_go'] for c in crawlers]==[
        'level6-200.json','level6-1490.json','level6-3078.json','level6-2670.json'], 'Changed HKQ1 Crawler source order'
    locks=m['camera_locks']
    gates=m['gates'];doors=m['doors'];hazards=m['hazards'];checkpoints=m['hazard_respawns']
    for group, selected in dict(crawlers=crawlers, camera_locks=locks, gates=gates,
                                doors=doors, hazards=hazards, hazard_respawns=checkpoints).items():
        require_coverage((r['source_go'] for r in m[group]),
                         (r['source_go'] for r in selected), 'HKQ1 '+group)
    def read(path):return json.loads(path.read_text())
    hero=read(ROOT/'_private/research/assets/c1-20602.json')['m_Structure']
    clips=read(ROOT/'_private/research/assets/c1-20600.json')['m_Structure']['clips']
    wake=next(c for c in clips if c['name']=='Wake Up Ground')
    getup=next(c for c in clips if c['name']=='Collect Normal 3')
    # viewport follow is a deliberate port choice until source camera controls
    # are measured; lock coordinates retain the source's authored limits.
    meta=[*m['initial_spawn'],*m['playable_respawn'],14.6,8.3,m['bounds'][2]-14.6,m['bounds'][3]-8.3,
          2.85+.8,len(wake['frames'])/wake['fps'],1,.15,
          sum(m['entry_semantics']['hidden_delays']),abs(hero['SPEED_TO_ENTER_SCENE_DOWN']),
          11.56,.75,3.25,len(getup['frames'])/getup['fps']]
    out=bytearray(struct.pack('<4s8I18f',b'HKQ1',len(crawlers),len(doors),len(locks),len(gates),3,
                              len(hazards),len(checkpoints),11,*meta))
    for c in crawlers:out+=struct.pack('<3fI',*c['spawn'],c['scale_x'],c['collider_count'])
    # Owner is obtained from the checked source collider, avoiding a second
    # manually maintained mapping for artwork activation.
    terrain=ROOT/'_private/research/terrain'
    for d in doors:
        owner=read(terrain/f"level6-{d['trigger']}.json")['m_GameObject']['m_PathID']
        ids=d['disabled_shapes']
        out+=struct.pack('<5I',d['trigger'],len(ids),d['layer'],int(d['persistent']),owner)
        out+=struct.pack('<'+'I'*len(ids),*ids)
    default=meta[4:8]
    for l in locks:
        limits=[default[i] if v==-1 else v for i,v in enumerate(l['limits'])]
        out+=struct.pack('<8fI',*l['rect'],*limits,int(l['prevent_up'])|int(l['prevent_down'])<<1|int(l['priority'])<<2)
    def string(v):
        raw=v.encode('ascii');assert len(raw)<32;return raw+b'\0'*(32-len(raw))
    for g in gates:
        out+=struct.pack('<7f',*g['rect'],*g['offset'],g['entry_delay'])
        out+=string(g['name'])+string(g['target'])+string(g['entry'])
    for h in hazards:out+=struct.pack('<4f2I',*h['rect'],h['damage'],h['type'])
    for c in checkpoints:out+=struct.pack('<6fI',*c['rect'],*c['position'],int(c['facing_right'])|int(c['fire_once'])<<1)
    # HKQ1 extension 1: source hazard coroutine/respawn and room collapse.
    research=ROOT/'_private/research/tutorial'
    # The hand-written identity note cross-checks the collapse shapes when a
    # development tree has it; Steam builds derive them from the room rules.
    collapse=m['collapse'];collapse_ids=list(collapse['disabled_shapes'])
    if (research/'collapse-identities.json').is_file():
        identities=read(research/'collapse-identities.json')
        assert [r['shape'] for r in identities['shapes']]==collapse_ids
    collapse_lock=next(i for i,l in enumerate(locks) if l['name']==collapse['camera_lock_name'])
    marker=read(research/'level6-12838.json')['m_Structure']['respawnMarker']['m_PathID']
    marker_data=read(research/f'level6-{marker}.json')
    go=read(research/f"level6-{marker_data['m_GameObject']['m_PathID']}.json")
    transforms=[]
    for component in go['m_Components']:
        candidate=read(research/f"level6-{component['m_Component']['m_PathID']}.json")
        if 'm_LocalPosition' in candidate:transforms=[candidate];break
    assert transforms
    while transforms[-1]['m_Father']['m_PathID']:
        transforms.append(read(research/f"level6-{transforms[-1]['m_Father']['m_PathID']}.json"))
    marker_point=transform_point([0,0],transforms)
    hazard_clip=next(c for c in clips if c['name']=='Hazard Respawn')
    hazard_values=[.8,len(hazard_clip['frames'])/hazard_clip['fps'],hero['INVUL_TIME']*2,50,
                   collapse['delay'],*marker_point]
    out+=struct.pack('<3I7f4f',len(collapse_ids),collapse_lock,int(marker_data['m_Structure']['respawnFacingRight']),
                     *hazard_values,*collapse['trigger_rect'])
    hurt=read(ROOT/'_private/research/enemy/resources.assets-13852.json')
    offset,size=hurt['m_Offset'],hurt['m_Size']
    hurtbox=[offset['m_X']-size['m_X']/2,offset['m_Y']-size['m_Y']/2,
             offset['m_X']+size['m_X']/2,offset['m_Y']+size['m_Y']/2]
    out+=struct.pack('<4f',*hurtbox)
    out+=struct.pack('<'+'I'*len(collapse_ids),*collapse_ids)
    contours=read(ROOT/'_private/converted/room-terrain/manifest.json')['shapes']
    floorIds=[8083,8261]
    terrainMap=read(ROOT/'_private/research/terrain/terrain-map.json')
    floorRow=next(r for r in terrainMap['inventory'] if r['collider']=='level6-8260.json')
    floorCollider=read(terrain/'level6-8260.json')
    floorRange=[transform_point(p,[read(terrain/k) for k in floorRow['chain']]) for path in paths(floorCollider,'BoxCollider2D') for p in path]
    floorRect=[min(p[0] for p in floorRange),min(p[1] for p in floorRange),max(p[0] for p in floorRange),max(p[1] for p in floorRange)]
    # Break Floor 1 break_floor: three accepted nail hits, first two .25 waits;
    # child range bool gates hits; final Activated destroys children.
    out+=struct.pack('<3I5f2I',8083,2,8,*floorRect,.25,*floorIds)
    visuals=ROOT/'_private/research/room-visuals'
    visual_map=read(visuals/'room-visuals-map.json')
    artwork=[]
    for d in doors:
        owner=read(terrain/f"level6-{d['trigger']}.json")['m_GameObject']['m_PathID']
        whole,fade,seconds=door_artwork(visuals,visual_map['inventory'],owner)
        out+=struct.pack('<2If',len(whole),len(fade),seconds)
        out+=struct.pack('<'+'I'*(len(whole)+len(fade)),*whole,*fade)
        artwork.append(dict(owner=owner,whole=whole,fade=fade,seconds=seconds))
    reveals=player_reveals(visuals,visual_map['inventory'])
    out+=struct.pack('<I',len(reveals))
    for reveal in reveals:
        out+=struct.pack('<2If',len(reveal['triggers']),len(reveal['owners']),reveal['seconds'])
        for trigger in reveal['triggers']:out+=struct.pack('<4f',*trigger)
        out+=struct.pack('<'+'I'*len(reveal['owners']),*reveal['owners'])
    remasks=(inverse_remasks(visuals,visual_map['inventory'])+player_remasks(visuals,visual_map['inventory'])+
             startup_inverse_remasks(visuals,visual_map['inventory']))+fury_remasks(visuals,visual_map['inventory'])+small_secret_remask(visuals,visual_map['inventory'])
    out+=struct.pack('<I',len(remasks))
    for mask in remasks:
        out+=struct.pack('<5fI',*mask['trigger'],mask['seconds'],len(mask['owners']))
        polygon=mask.get('polygon',[])
        out+=struct.pack('<2I',int(mask.get('opaque_inside',True)),len(polygon))
        for point in polygon:out+=struct.pack('<2f',*point)
        out+=struct.pack('<'+'I'*len(mask['owners']),*mask['owners'])
        out+=struct.pack('<2fI',mask.get('startup_pause',0),mask.get('startup_fade',0),int(mask.get('startup_hero_positioned',False)))
    for c in crawlers:
        a=c.get('activation');flags=int(c['active'])|(2 if a else 0)|(4 if a and a['disable_if_visited_dirtmouth'] else 0)
        out+=struct.pack('<I5f',flags,*(a['trigger'] if a else [0]*4),a['ultrawide_threshold'] if a else 0)
    messages=load_messages(terrain)
    out+=struct.pack('<I',len(messages))
    for zone in messages:
        out+=struct.pack('<4f2I',*zone['rect'],zone['layers'],('GO LEFT','GO RIGHT','GO UP').index(zone['event']))
    secrets=secret_regions(visuals,visual_map['inventory'])
    out+=struct.pack('<I',len(secrets))
    for secret in secrets:out+=struct.pack('<4f2I2f',*secret['trigger'],secret['owner'],secret['save_flag'],secret['reload_fade'],secret['discovery_fade'])
    floor_owners=sorted(int(r['go'][7:-5]) for r in visual_map['inventory'] if r['active'] and 'level6-2421.json' in r['owners'])
    assert floor_owners==[2422,2423,2424,2426], 'Changed Break Floor 1 visual descendants'
    out+=struct.pack('<I4I',len(floor_owners),*floor_owners)
    target=ROOT/'_private/converted/hero/romfs/room-rules.bin';target.write_bytes(out)
    (source.parent/'rules-manifest.json').write_text(json.dumps(dict(binary_sha256=hashlib.sha256(out).hexdigest(),
        source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),bytes=len(out),object_inventory=inventory,enemy_messages=messages,
        break_floor_owners=floor_owners,hazard_values=hazard_values,collapse_shapes=collapse_ids,collapse_camera=collapse_lock,door_artwork=artwork,player_reveals=reveals,inverse_remasks=remasks,secret_regions=secrets,crawler_activation=[dict(name=c['name'],initial_active=c['active'],activation=c.get('activation')) for c in crawlers],
        artwork_source_sha256=hashlib.sha256((visuals/'room-visuals-map.json').read_bytes()).hexdigest(),
        limitations=['Collapse floor tilt, debris/particles/audio, fade visualization and save I/O are not implemented.', 'Viewport uses port exponential follow; original two-stage camera smoothing differs.']),indent=2))
    print(json.dumps(dict(bytes=len(out),crawlers=len(crawlers),doors=len(doors),locks=len(locks))))


if __name__=='__main__':convert()
