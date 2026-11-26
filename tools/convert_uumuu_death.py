"""Validate the Uumuu corpse's save/broadcast boundary and generate its waits."""
import converted_files  # Install copy-on-write protection before output writes.
import argparse
import json
from pathlib import Path
from convert_uumuu import literal


def action(state, kind):
    values=[a['parameters'] for a in state['actions'] if a['type'].split('.')[-1]==kind]
    if len(values)!=1:
        raise ValueError(f'Expected one {kind} in {state["name"]}')
    return values[0]


def text(value):
    if value['useVariable']:
        raise ValueError('Expected literal event/PlayerData name')
    return value['value']


def convert(source, snapshot_code, boss_code):
    if source['build_id']!=22529139 or source['source_reference']!='level229 external3 path32':
        raise ValueError('Unsupported Uumuu corpse source')
    actors=[a for a in source['actors'] if a['name']=='Corpse Mega Jellyfish']
    if len(actors)!=1:
        raise ValueError('Expected one Uumuu corpse')
    fsms=[c['decoded'] for c in actors[0]['components'] if c['type']=='PlayMakerFSM']
    if len(fsms)!=1 or fsms[0]['name']!='Death' or fsms[0]['start']!='PlayerData':
        raise ValueError('Unsupported corpse FSM')
    fsm=fsms[0];states={s['name']:s for s in fsm['states']}
    order=['PlayerData','Music','Init','Steam','Ready','Blow']
    if set(states)!=set(order) or fsm['globalTransitions']:
        raise ValueError('Unsupported corpse states')
    for i,name in enumerate(order):
        expected=[] if i==len(order)-1 else [('FINISHED',order[i+1])]
        state=states[name]
        edges=[(t['fsmEvent']['name'],t['toState']) for t in state['transitions']]
        if edges!=expected or any(not a['enabled'] for a in state['actions']):
            raise ValueError(f'Unsupported corpse transitions/actions in {name}')
    counts=[len(states[n]['actions']) for n in order]
    if counts!=[1,2,8,12,6,11]:
        raise ValueError('Unsupported corpse action counts')
    setter=action(states['PlayerData'],'SetPlayerDataBool')
    if text(setter['boolName'])!='defeatedMegaJelly' or literal(setter['value']) is not True:
        raise ValueError('Unsupported Uumuu victory setter')
    check=action(states['Music'],'GGCheckIfBossScene')
    if check!=dict(bossSceneEvent='FINISHED',regularSceneEvent=''):
        raise ValueError('Unsupported music skip')
    snapshot=action(states['Music'],'TransitionToAudioSnapshot')
    # Both actions Finish on entry. Snapshot interpolation does not delay Init.
    if ('audioMixerSnapshot.TransitionTo(transitionTime.Value);' not in snapshot_code or
            'Finish();' not in snapshot_code or 'WaitFor' in snapshot_code or
            'base.Fsm.Event(bossSceneEvent);' not in boss_code or
            'base.Fsm.Event(regularSceneEvent);' not in boss_code or 'Finish();' not in boss_code):
        raise ValueError('Unsupported corpse music action implementation')
    facts=dict(snapshotTime=literal(snapshot['transitionTime']))
    for state,name in [('Init','initWait'),('Steam','steamWait'),('Ready','readyWait')]:
        wait=action(states[state],'Wait')
        duration=literal(wait['time'])
        if wait['finishEvent']!='FINISHED' or wait['realTime'] or duration<=0:
            raise ValueError('Unsupported corpse scaled wait')
        facts[name]=duration
    # First the child/self effects event, then the encounter-wide BOSS DEATH.
    for state,event in [('Steam','CORPSE STEAM'),('Ready','CORPSE READY'),('Blow','CORPSE BLOW')]:
        a=states[state]['actions'][0]
        p=a['parameters']
        if a['type'].split('.')[-1]!='SendEventByName' or text(p['sendEvent'])!=event or p['eventTarget']['target']!=1:
            raise ValueError('Unsupported corpse phase event')
    a=states['Blow']['actions'][1];p=a['parameters']
    if (a['type'].split('.')[-1]!='SendEventByName' or text(p['sendEvent'])!='BOSS DEATH' or
            p['eventTarget']['target']!=4 or literal(p['delay'])!=0 or p['everyFrame']):
        raise ValueError('Unsupported BOSS DEATH broadcast')
    return facts


def header(facts):
    rows=['// Generated from build22529139 level229 external3/path32, corpse Death FSM.',
          '#pragma once','namespace hk3ds::uumuu_death_data {']
    for name in ('initWait','steamWait','readyWait','snapshotTime'):
        rows.append(f'inline constexpr float {name}={facts[name]:.9f}f;')
    return '\n'.join(rows+['}',''])


def main():
    root=Path(__file__).resolve().parents[1]
    code=root/'_private/research/decompiled/all'
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,default=root/'_private/minimum-bosses/uumuu/corpse-source.json')
    parser.add_argument('--snapshot-code',type=Path,default=code/'HutongGames.PlayMaker.Actions/TransitionToAudioSnapshot.cs')
    parser.add_argument('--boss-code',type=Path,default=code/'GGCheckIfBossScene.cs')
    parser.add_argument('--output',type=Path,default=root/'include/uumuu_death_data.hpp')
    args=parser.parse_args()
    facts=convert(json.loads(args.source.read_text()),args.snapshot_code.read_text(),args.boss_code.read_text())
    args.output.write_text(header(facts));print(json.dumps(facts))


if __name__=='__main__':main()
