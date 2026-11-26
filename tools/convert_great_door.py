"""Convert the private Tutorial_01 great-door FSM into a bounded native GDR1 record."""
import converted_files  # Install hardlink copy-on-write before asset writes.
import argparse
import hashlib
import json
import math
import struct
from pathlib import Path
from inspect_fsm import decode_fsm
from private_output_paths import is_private_output
ROOT=Path(__file__).resolve().parents[1]


def convert(source,output):
    # Shared research may be a worktree junction; keep checkout-relative provenance.
    source=source.absolute();output=output.absolute()
    if not converted_files.private_output(output, ROOT):
        raise ValueError('Original-derived door records must remain private')
    mapping=json.loads((source/'sources.json').read_text())
    hashes={}
    def read(name):
        path=source/name;raw=path.read_bytes();sha=hashlib.sha256(raw).hexdigest()
        if sha!=mapping['sha256'][name]:raise ValueError('Changed source '+name)
        hashes[str(path.relative_to(ROOT))]=sha
        return json.loads(raw)
    def component(kind):return read(mapping['parts'][kind][0])
    rawFsm=component('PlayMakerFSM')['m_Structure']['fsm']
    fsm=decode_fsm(rawFsm);states={s['name']:s for s in fsm['states']}
    def action(state,kind):
        matches=[a for a in states[state]['actions'] if a['enabled'] and a['type'].split('.')[-1]==kind]
        if len(matches)!=1:raise ValueError('Expected one '+kind+' in '+state)
        return matches[0]
    def value(p,key):
        v=p[key]
        if v.get('useVariable') or 'variable' in v:raise ValueError('Variable '+key)
        return v['value']
    def array(a,key):
        ps=a['ordered_parameters'];i=next(i for i,p in enumerate(ps) if p['name']==key)+1
        result=[]
        while i<len(ps) and ps[i]['name']=='':
            result.append(ps[i]['value']);i+=1
        return result
    expected={
        'Pause':{'FINISHED':'Initiate'},'Initiate':{'FINISHED':'Idle','ACTIVATE':'Activate'},
        'Idle':{'HIT':'Check If Nail'},'Check If Nail':{'TRUE':'Hit','FINISHED':'Idle'},
        'Hit':{'FINISHED':'Check Hits'},
        'Check Hits':{'BREAK1':'Break 1','BREAK2':'Break 2','BREAK3':'Break','FINISHED':'Idle'},
        'Break 1':{'FINISHED':'Idle'},'Break 2':{'FINISHED':'Idle'},
        'Break':{'RIGHT':'Extra Frame'},'Extra Frame':{'FINISHED':'Move'},'Move':{},'Activate':{}}
    if set(states)!=set(expected) or fsm['start']!='Pause' or rawFsm['globalTransitions']:
        raise ValueError('Door FSM topology changed')
    for n,transitions in expected.items():
        if {t['fsmEvent']['name']:t['toState'] for t in states[n]['transitions']}!=transitions:
            raise ValueError('Door transitions changed '+n)
    received=action('Idle','ReceivedDamage')['parameters']
    if received['fsmName']['value']!='damages_enemy' or received['sendEvent']!='HIT':
        raise ValueError('Door damage event source changed')
    persistent=component('PersistentBoolItem')['m_Structure']
    if persistent['semiPersistent'] or persistent['dontSave']:raise ValueError('Door persistence semantics changed')
    for state,event in [('Pause','FINISHED'),('Break','RIGHT'),('Extra Frame','FINISHED')]:
        if action(state,'NextFrameEvent')['parameters']['sendEvent']!=event:raise ValueError('Door NextFrame event changed')
    switch=action('Check Hits','IntSwitch')
    thresholds=[v['value'] for v in array(switch,'compareTo')]
    if array(switch,'sendEvent')!=['BREAK1','BREAK2','BREAK3'] or len(thresholds)!=3:
        raise ValueError('Door hit stages changed')
    if [v['value'] for v in array(action('Check If Nail','IntSwitch'),'compareTo')]!=[0]:
        raise ValueError('Door attack filter changed')
    add=action('Check Hits','IntAdd')['parameters']
    if value(add,'add')!=1:raise ValueError('Door hit increment changed')
    waits=[]
    for n in ('Check Hits','Break 1','Break 2'):
        p=action(n,'Wait')['parameters']
        if p['realTime'] or p['finishEvent']!='FINISHED':raise ValueError('Door wait semantics changed')
        waits.append(value(p,'time'))
    if len(set(waits))!=1:raise ValueError('Door stages use different waits')
    if any(a['type'].split('.')[-1] in ('SetCollider','SetMeshRenderer') for a in states['Break']['actions'] if a['enabled']):
        raise ValueError('Fresh Break activation behavior changed')
    for kind in ('SetCollider','SetMeshRenderer'):
        if value(action('Activate',kind)['parameters'],'active'):raise ValueError('Persisted Activate no longer hides door')
    if not value(action('Break','SetBoolValue')['parameters'],'boolValue'):raise ValueError('Door no longer persists activation')
    transition=action('Move','BeginSceneTransition')['parameters']
    scene=transition['sceneName']['value'];gate=transition['entryGateName']['value'];delay=value(transition,'entryDelay')
    collider=component('BoxCollider2D');go=component('GameObject')
    if collider['m_IsTrigger'] or not collider['m_Enabled']:raise ValueError('Door solid collider changed')
    terrainId=int(mapping['parts']['BoxCollider2D'][0].split('-')[-1].split('.')[0])
    tutorial=ROOT/'_private/research/tutorial'
    inventory=json.loads((tutorial/'inventory.json').read_text())
    row=next(r for r in inventory['rows'] if r['collider']==mapping['parts']['BoxCollider2D'][0])
    x=y=0
    for name in reversed(row['chain']):
        path=tutorial/name;raw=path.read_bytes();sha=hashlib.sha256(raw).hexdigest()
        if sha!=inventory['source_sha256'][name]:raise ValueError('Changed transform '+name)
        hashes[str(path.relative_to(ROOT))]=sha;t=json.loads(raw)
        if t['m_LocalRotation']!={'m_W':1,'m_X':0,'m_Y':0,'m_Z':0} or t['m_LocalScale']!={'m_X':1,'m_Y':1,'m_Z':1}:
            raise ValueError('Rotated/scaled door needs explicit support')
        x+=t['m_LocalPosition']['m_X'];y+=t['m_LocalPosition']['m_Y']
    ox=collider['m_Offset']['m_X'];oy=collider['m_Offset']['m_Y'];sx=collider['m_Size']['m_X'];sy=collider['m_Size']['m_Y']
    bounds=[x+ox-sx/2,y+oy-sy/2,x+ox+sx/2,y+oy+sy/2]
    if not all(math.isfinite(v) for v in bounds+[waits[0],delay]):raise ValueError('Nonfinite door data')
    for method in ['HutongGames.PlayMaker.Actions/ReceivedDamage.cs','HutongGames.PlayMaker.Actions/BeginSceneTransition.cs','PersistentBoolItem.cs']:
        path=ROOT/'_private/research/decompiled/all'/method;hashes[str(path.relative_to(ROOT))]=hashlib.sha256(path.read_bytes()).hexdigest()
    def text(s):
        raw=s.encode('utf-8')
        if len(raw)>=48 or b'\0' in raw:raise ValueError('Door transition string too long')
        return raw.ljust(48,b'\0')
    data=struct.pack('<4s5I6f',b'GDR1',terrainId,go['m_Layer'],*thresholds,*bounds,waits[0],delay)+text(scene)+text(gate)
    output.mkdir(parents=True,exist_ok=True);(output/'great-door.bin').write_bytes(data)
    report=dict(build_id=22529139,terrain_id=terrainId,bounds=bounds,thresholds=thresholds,hit_wait=waits[0],transition=dict(scene=scene,gate=gate,entry_delay=delay,next_frame_events=2),stage_sprite_names=[action(n,'Tk2dSpriteSetId')['parameters']['ORSpriteName']['value'] for n in ('Break 1','Break 2')],fresh_break_disables_collider=False,persisted_activate_disables_collider=True,source_sha256=hashes,sha256=hashlib.sha256(data).hexdigest(),confidence='High for serialized FSM/config; frame ordering and callbacks source-derived, not runtime trace compared',limitations=['VFX/debris/audio/camera fade presented by adapter','EntryDelay is destination scene entry metadata, not a local countdown','No runtime slash-count probe completed'])
    (output/'great-door-manifest.json').write_text(json.dumps(report,indent=2))
    return report


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source',type=Path,default=ROOT/'_private/research/great-door')
    p.add_argument('--output',type=Path,default=ROOT/'_private/converted/tutorial')
    a=p.parse_args();r=convert(a.source,a.output)
    print(json.dumps({k:r[k] for k in ('terrain_id','thresholds','sha256')}))
