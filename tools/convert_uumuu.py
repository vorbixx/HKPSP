"""Generate only proven Uumuu shell/battle facts from the copied scene snapshot."""
import converted_files  # Install copy-on-write protection before output writes.
import argparse
import json
import math
from pathlib import Path
from watcher_battle import component_data, find_components, state_action


def one(values, description):
    if len(values)!=1:
        raise ValueError(f'Expected exactly one {description}')
    return values[0]


def literal(value):
    if isinstance(value, dict):
        if value.get('useVariable'):
            raise ValueError('Runtime variable cannot be used as constant tuning')
        value=value['value']
    if not isinstance(value,(int,float)) or not math.isfinite(value):
        raise ValueError('Expected finite authored numeric/bool value')
    return value


def integer(value, description, minimum=0):
    if type(value) is not int or value<minimum:
        raise ValueError(f'Expected authored integer {description} >= {minimum}')
    return value


def boolean(value, description):
    # Unity typetrees serialize bool fields as 0/1; decoded FSM literals use bool.
    if type(value) not in (bool,int) or value not in (0,1):
        raise ValueError(f'Expected authored boolean {description}')
    return bool(value)


def battle_contract(battle):
    def state(name):return one([s for s in battle['states'] if s['name']==name],name)
    for name,event,target in (('Idle','ENTER','Start'),('Idle','ACTIVATE','Activate'),
                              ('Start','BOSS DEATH','End Pause'),('End Pause','FINISHED','End')):
        transition=one([t for t in state(name)['transitions'] if t['fsmEvent']['name']==event],event)
        if transition['toState']!=target:
            raise ValueError(f'Unsupported Uumuu battle transition {name}/{event}')
    defeated=state_action(battle,'Idle','PlayerDataBoolTest')
    encountered=state_action(battle,'Start','SetPlayerDataBool')
    if (defeated['boolName']['value']!='defeatedMegaJelly' or defeated['isTrue']!='ACTIVATE' or
            defeated['isFalse'] or encountered['boolName']['value']!='encounteredMegaJelly' or
            literal(encountered['value']) is not True):
        raise ValueError('Unsupported Uumuu battle save interface')
    for name,active,broadcasts in (('Start',True,['BG CLOSE','BATTLE START']),
                                    ('End',False,['BG OPEN']),('Activate',False,['BG QUICK OPEN'])):
        children=state_action(battle,name,'ActivateAllChildren')
        events=[a['parameters'] for a in state(name)['actions'] if a['enabled'] and
                a['type'].split(',')[0].split('.')[-1]=='SendEventByName']
        if (boolean(children['activate'],'activate children') is not active or
                [e['sendEvent']['value'] for e in events]!=broadcasts or
                any(literal(e['delay'])!=0 or e['everyFrame'] for e in events)):
            raise ValueError(f'Unsupported Uumuu battle outputs in {name}')


def quirrel_facts(control):
    def initial(kind,name):
        return one([v for v in control['variables'][kind] if v['name']==name],name)['value']
    def state(name):return one([s for s in control['states'] if s['name']==name],name)
    def transition(name,event,target):
        edge=one([t for t in state(name)['transitions'] if t['fsmEvent']['name']==event],event)
        if edge['toState']!=target:
            raise ValueError(f'Unsupported Quirrel transition {name}/{event}')
    def land(action):
        target=action['gameObject']
        if (target['ownerOption']!=1 or not target['gameObject']['useVariable'] or
                target['gameObject']['name']!='Quirrel Land'):
            raise ValueError('Unsupported Quirrel placement/animation actor')
    def slash_actor(target):
        if 'ownerOption' in target:
            if target['ownerOption']!=1:
                raise ValueError('Unsupported Quirrel Slash actor target')
            target=target['gameObject']
        if not target['useVariable'] or target['name']!='Quirrel Slash':
            raise ValueError('Unsupported Quirrel Slash actor target')
    counter=state_action(control,'Idle','FloatAdd')
    if (counter['floatVariable']['variable']!='Quirrel Timer' or
            not counter['everyFrame'] or not counter['perSecond']):
        raise ValueError('Unsupported Quirrel idle timer')
    reset=state_action(control,'Set Timer','SetFloatValue')
    random=state_action(control,'Set Timer','RandomFloat')
    if (reset['floatVariable']['variable']!='Quirrel Timer' or reset['everyFrame'] or
            random['storeResult']['variable']!='Quirrel Time'):
        raise ValueError('Unsupported Quirrel timer reset')
    compare=state_action(control,'Quirrel?','FloatCompare')
    test=state_action(control,'Quirrel?','BoolTest')
    if (compare['float1']['variable']!='Quirrel Timer' or
            compare['float2']['variable']!='Quirrel Time' or literal(compare['tolerance'])!=0 or
            compare['equal']!='QUIRREL' or compare['greaterThan']!='QUIRREL' or
            compare['lessThan'] or compare['everyFrame'] or
            test['boolVariable']['variable']!='Quirrel Arrived' or test['isTrue'] or
            test['isFalse']!='FINISHED' or test['everyFrame']):
        raise ValueError('Unsupported Quirrel eligibility branch')
    watch=state_action(control,'Quirrel?','SendEventByName')
    target=watch['eventTarget']
    if (target['target']!=1 or target['gameObject']['gameObject']['name']!='Quirrel Land' or
            target['fsmName']['value'] or literal(watch['delay'])!=0 or watch['everyFrame'] or
            watch['sendEvent']['value']!='WATCH'):
        raise ValueError('Unsupported targeted Quirrel WATCH event')
    for name,enabled in (('Quirrel Roam',True),('Arrive Pause',False)):
        roam=state_action(control,name,'SetFsmBool')
        if (roam['gameObject']['gameObject']['name']!='Quirrel Land' or
                roam['fsmName']['value']!='Watch' or roam['variableName']['value']!='Roam' or
                literal(roam['setValue']) is not enabled or roam['everyFrame']):
            raise ValueError('Unsupported Quirrel Roam handoff')
    arrived=state_action(control,'Arrive Pause','SetBoolValue')
    arrived_test=state_action(control,'Arrive Pause','BoolTest')
    if (arrived['boolVariable']['variable']!='Quirrel Arrived' or
            literal(arrived['boolValue']) is not True or arrived['everyFrame'] or
            arrived_test['boolVariable']['variable']!='Quirrel Arrived' or
            arrived_test['isTrue']!='ATTACK' or arrived_test['isFalse'] or arrived_test['everyFrame']):
        raise ValueError('Unsupported Quirrel arrival branch')
    result=dict(quirrelInitialTimer=literal(initial('floatVariables','Quirrel Timer')),
                quirrelInitialTime=literal(initial('floatVariables','Quirrel Time')),
                quirrelInitiallyArrived=boolean(initial('boolVariables','Quirrel Arrived'),'Quirrel Arrived'),
                quirrelIdleTimerRate=literal(counter['add']),quirrelTimerReset=literal(reset['floatValue']),
                quirrelTimeMin=literal(random['min']),quirrelTimeMax=literal(random['max']))
    arrival_types=[a['type'].split(',')[0].split('.')[-1]
                   for a in state('Arrive Pause')['actions'] if a['enabled']]
    if arrival_types!=['SetFsmBool','BoolTest','SetBoolValue','ChaseObjectV2','Wait']:
        raise ValueError('Unsupported Quirrel arrival action order')
    for name,key,event,target in (
            ('Arrive Pause','quirrelArrivePauseWait','FINISHED','Check Pos'),
            ('Attack Pause','quirrelAttackPauseWait','FINISHED','Slash Antic'),
            ('Q Arrive','quirrelArriveWait','ATTACK','Q Leave'),
            ('Slash Pause','quirrelSlashPauseWait','FINISHED','Slash Antic')):
        wait=state_action(control,name,'Wait')
        value=literal(wait['time'])
        if value<=0 or wait['realTime'] or wait['finishEvent']!=event:
            raise ValueError(f'Unsupported Quirrel wait {name}')
        transition(name,event,target)
        result[key]=value
    transition('Arrive Pause','ATTACK','Attack Pause')
    # Q Leave finishes through its authored animation event, never a guessed wait.
    leave=state_action(control,'Q Leave','Tk2dPlayAnimationWithEvents')
    land(leave)
    if (leave['clipName']['value']!='Quirrel Leave' or leave['animationTriggerEvent'] or
            leave['animationCompleteEvent']!='FINISHED' or
            any(a['enabled'] and a['type'].split(',')[0].split('.')[-1]=='Wait'
                for a in state('Q Leave')['actions'])):
        raise ValueError('Unsupported Quirrel leave animation boundary')
    transition('Q Leave','FINISHED','Slash Pause')
    for axis in ('X','Y'):
        result['quirrelHeroMid'+axis]=literal(initial('floatVariables','Hero Mid '+axis))
    for corner in ('BL','BR','TL','TR'):
        position=state_action(control,corner,'SetPosition')
        scale=state_action(control,corner,'SetScale')
        for placement in (position,scale):
            land(placement)
            if (placement['everyFrame'] or placement['lateUpdate'] or
                    not placement['vector']['useVariable'] or not placement['z']['useVariable']):
                raise ValueError('Unsupported Quirrel placement axes/update')
        if position['space']!=0 or not scale['y']['useVariable']:
            raise ValueError('Unsupported Quirrel placement space/scale')
        transition(corner,'FINISHED','Q Arrive')
        result['quirrel'+corner+'X']=literal(position['x'])
        result['quirrel'+corner+'Y']=literal(position['y'])
        result['quirrel'+corner+'Scale']=literal(scale['x'])
    if result['quirrelTimeMin']<=0 or result['quirrelTimeMax']<result['quirrelTimeMin']:
        raise ValueError('Invalid authored Quirrel time range')
    # Slash Antic mutates Quirrel Slash after both authored audio actions and
    # transitions only on the actual animation FINISHED event.
    slash_state=state('Slash Antic')
    slash_types=[a['type'].split(',')[0].split('.')[-1]
                 for a in slash_state['actions'] if a['enabled']]
    if slash_types!=['DecelerateV2','AudioPlayerOneShot','AudioPlayerOneShotSingle',
                     'RandomFloat','RandomFloatEither','SetRotation','SetScale',
                     'SetMeshRenderer','Tk2dPlayAnimationWithEvents']:
        raise ValueError('Unsupported Quirrel Slash Antic action order')
    decelerate=state_action(control,'Slash Antic','DecelerateV2')
    decel_target=decelerate['gameObject']
    decel_object=decel_target['gameObject']
    if (decel_target['ownerOption']!=0 or decel_object['useVariable'] or decel_object['name'] or
            decel_object['value']!={'m_FileID':0,'m_PathID':0} or
            literal(decelerate['deceleration'])!=0.8500000238418579):
        raise ValueError('Unsupported Quirrel Slash Antic deceleration')
    one_shot=state_action(control,'Slash Antic','AudioPlayerOneShot')
    one_single=state_action(control,'Slash Antic','AudioPlayerOneShotSingle')
    for audio in (one_shot,one_single):
        slash_actor(audio['spawnPoint'])
        player=audio['audioPlayer']
        if player['useVariable'] or player['value']!={'m_FileID':4,'m_PathID':4126}:
            raise ValueError('Unsupported Quirrel Slash audio player')
        if (literal(audio['pitchMin'])!=1 or literal(audio['pitchMax'])!=1 or
                literal(audio['volume'])!=1 or literal(audio['delay'])!=0):
            raise ValueError('Unsupported Quirrel Slash audio settings')
    clips=one_shot['audioClips']['elements']
    weights=one_shot['weights']['elements']
    if ([(c['m_FileID'],c['m_PathID']) for c in clips]!=[(3,16),(3,15)] or
            [literal(w) for w in weights]!=[1,1]):
        raise ValueError('Unsupported Quirrel Slash weighted audio action')
    if one_single['audioClip']['value']!={'m_FileID':3,'m_PathID':7}:
        raise ValueError('Unsupported Quirrel Slash single audio action')
    rotation=state_action(control,'Slash Antic','RandomFloat')
    scale=state_action(control,'Slash Antic','RandomFloatEither')
    if rotation['storeResult']['variable']!='Slash Rotation' or scale['storeResult']['variable']!='Slash Scale':
        raise ValueError('Unsupported Quirrel Slash random variable binding')
    result['quirrelSlashRotationMin']=literal(rotation['min'])
    result['quirrelSlashRotationMax']=literal(rotation['max'])
    result['quirrelSlashScale1']=literal(scale['value1'])
    result['quirrelSlashScale2']=literal(scale['value2'])
    if result['quirrelSlashRotationMin']>result['quirrelSlashRotationMax']:
        raise ValueError('Invalid Quirrel Slash rotation range')
    set_rotation=state_action(control,'Slash Antic','SetRotation')
    slash_actor(set_rotation['gameObject'])
    if (set_rotation['zAngle'].get('variable')!='Slash Rotation' or
            set_rotation['space']!=1 or set_rotation['everyFrame'] or set_rotation['lateUpdate']):
        raise ValueError('Unsupported Quirrel Slash local Z rotation')
    set_scale=state_action(control,'Slash Antic','SetScale')
    slash_actor(set_scale['gameObject'])
    if (set_scale['x'].get('variable')!='Slash Scale' or
            not set_scale['y']['useVariable'] or not set_scale['z']['useVariable'] or
            set_scale['everyFrame'] or set_scale['lateUpdate']):
        raise ValueError('Unsupported Quirrel Slash X scale')
    renderer=state_action(control,'Slash Antic','SetMeshRenderer')
    slash_actor(renderer['gameObject'])
    animation=state_action(control,'Slash Antic','Tk2dPlayAnimationWithEvents')
    slash_actor(animation['gameObject'])
    if (literal(renderer['active']) is not True or animation['clipName']['value']!='Slash Antic' or
            animation['animationTriggerEvent'] or animation['animationCompleteEvent']!='FINISHED'):
        raise ValueError('Unsupported Quirrel Slash Antic actor output')
    transition('Slash Antic','FINISHED','Slash')
    # These variables are float fields even when a typetree writes an integral value.
    return {k:v if isinstance(v,bool) else float(v) for k,v in result.items()}


def bounce_shroom_facts(jelly,control):
    component=one(find_components(jelly,'BounceShroom'),'Uumuu BounceShroom')
    data=component_data(component)
    if (jelly['path_id']!=15 or component['path_id']!=157 or
            data['m_GameObject']!={'m_FileID':0,'m_PathID':15}):
        raise ValueError('Unsupported initial Uumuu BounceShroom owner')
    initial=boolean(data['m_Enabled'],'BounceShroom enabled')
    if not initial:
        raise ValueError('Expected initially enabled Uumuu BounceShroom')
    def self_target(target):
        obj=target['gameObject']
        if (target['ownerOption']!=0 or obj['useVariable'] or obj['name'] or
                obj['value']!={'m_FileID':0,'m_PathID':0}):
            raise ValueError('Unsupported Uumuu BounceShroom action target')
    result=dict(initialBounceShroom=initial)
    for name,kind,key in (('Slash','DestroyComponent','slashBounceShroom'),
                          ('Recover','AddComponent','recoverBounceShroom')):
        action=state_action(control,name,kind)
        self_target(action['gameObject'])
        self_target(state_action(control,name,'SetInvincible')['target'])
        if action['component']['useVariable'] or action['component']['value']!='BounceShroom':
            raise ValueError('Unsupported Uumuu shell component')
        state=one([s for s in control['states'] if s['name']==name],name)
        relevant=[a['type'].split(',')[0].split('.')[-1] for a in state['actions']
                  if a['enabled'] and a['type'].split(',')[0].split('.')[-1]
                  in ('SetInvincible','DestroyComponent','AddComponent')]
        if relevant!=['SetInvincible',kind]:
            raise ValueError('Unsupported Uumuu shell component action order')
        if kind=='AddComponent':
            store=action['storeComponent']
            if (literal(action['removeOnExit']) is not False or not store['useVariable'] or
                    store['name'] or store['value']!={'m_FileID':0,'m_PathID':0}):
                raise ValueError('Unsupported Uumuu BounceShroom addition lifetime/store')
        # These describe named FSM action boundaries, not Unity destruction timing.
        result[key]=kind=='AddComponent'
    return result


def convert(source):
    if source['build_id']!=22529139 or source['scene']!='level229':
        raise ValueError('Expected copied build22529139 level229')
    def actor(name):return one([a for a in source['actors'] if a['name']==name],name)
    def fsm(owner,name):return one([c['decoded'] for c in owner['components'] if c.get('decoded',{}).get('name')==name],name)
    jelly=actor('Mega Jellyfish')
    health=component_data(one(find_components(jelly,'HealthManager'),'Uumuu HealthManager'))
    # Normal HealthManager.Die calls EnemyDeathEffects.RecieveDeathEvent.
    # This is a real corpse activation request, not a replacement victory event.
    effects=component_data(one(find_components(jelly,'EnemyDeathEffects'),'Uumuu death effects'))
    if (boolean(health['hasSpecialDeath'],'hasSpecialDeath') or
            effects['corpsePrefab']!={'m_FileID':3,'m_PathID':32} or
            boolean(effects['isCorpseRecyclable'],'isCorpseRecyclable') or
            effects['corpseSpawnPoint']!={'x':0.0,'y':0.5,'z':0.0}):
        raise ValueError('Unsupported Uumuu lethal-to-corpse contract')
    control=fsm(jelly,'Mega Jellyfish')
    battle=fsm(actor('Battle Scene'),'Control')
    battle_contract(battle)
    slash=state_action(control,'Slash','SetInvincible')
    recover=state_action(control,'Recover','SetInvincible')
    if literal(slash['Invincible']) is not False or literal(recover['Invincible']) is not True:
        raise ValueError('Unexpected authored shell actions')
    # Preserve the animation event boundary; never replace it with a fixed wait.
    animation=state_action(control,'Slash','Tk2dPlayAnimationWithEvents')
    if animation['animationCompleteEvent']!='FINISHED':
        raise ValueError('Unexpected Slash animation completion event')
    for state,target in (('Slash','Wounded'),('Wounded','Recover Antic'),('Recover Antic','Recover')):
        s=one([s for s in control['states'] if s['name']==state],state)
        transitions=[t for t in s['transitions'] if t['fsmEvent']['name']=='FINISHED']
        if one(transitions,f'{state} FINISHED')['toState']!=target:
            raise ValueError(f'Unexpected {state} transition')
    def wait(fsm,name):
        action=state_action(fsm,name,'Wait')
        value=literal(action['time'])
        if value<=0 or action['realTime'] or action['finishEvent']!='FINISHED':
            raise ValueError(f'Unsupported {name} wait')
        return value
    return dict(hp=integer(health['hp'],'hp',1),enemyType=integer(health['enemyType'],'enemyType'),
                initialInvincible=boolean(health['invincible'],'invincible'),
                initialDirection=integer(health['invincibleFromDirection'],'invincibleFromDirection'),
                slashDirection=integer(literal(slash['InvincibleFromDirection']),'slash direction'),
                recoverDirection=integer(literal(recover['InvincibleFromDirection']),'recover direction'),
                woundedWait=wait(control,'Wounded'),recoverAnticWait=wait(control,'Recover Antic'),
                recoverWait=wait(control,'Recover'),battleEndWait=wait(battle,'End Pause'),
                serializedInvulnerableTime=float(literal(health['invulnerableTime'])),
                **bounce_shroom_facts(jelly,control),
                **quirrel_facts(control))


def header(facts):
    rows=['// Generated by tools/convert_uumuu.py from copied build22529139 level229.',
          '#pragma once','namespace hk3ds::uumuu_data {']
    for name,value in facts.items():
        if isinstance(value,bool):kind,text='bool',str(value).lower()
        elif isinstance(value,int):kind,text='int',str(value)
        else:kind,text='float',f'{value:.9f}f'
        if name=='serializedInvulnerableTime':
            rows.append('// Serialized metadata only; combat owns its hit-evasion clock.')
        if name=='slashBounceShroom':
            rows.append('// Named FSM action boundaries; no assumption about Unity destruction timing.')
        rows.append(f'inline constexpr {kind} {name}={text};')
    return '\n'.join(rows+['}',''])


def main():
    root=Path(__file__).resolve().parents[1]
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,default=root/'_private/minimum-bosses/uumuu/level229-source.json')
    parser.add_argument('--output',type=Path,default=root/'include/uumuu_data.hpp')
    args=parser.parse_args()
    facts=convert(json.loads(args.source.read_text()))
    args.output.write_text(header(facts))
    print(json.dumps(facts))


if __name__=='__main__':main()
