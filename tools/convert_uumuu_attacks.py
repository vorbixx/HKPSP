"""Convert Uumuu's authored attack scheduler, without inventing lightning assets."""
import converted_files  # Install copy-on-write protection before output writes.
import argparse
import json
from pathlib import Path

from convert_uumuu import boolean, integer, literal, one
from watcher_battle import state_action


def convert(source):
    actor = one([a for a in source['actors'] if a['name'] == 'Mega Jellyfish'], 'Mega Jellyfish')
    control = one([c['decoded'] for c in actor['components']
                   if c.get('decoded', {}).get('name') == 'Mega Jellyfish'], 'Mega Jellyfish FSM')
    def state(name):
        return one([s for s in control['states'] if s['name'] == name], name)
    def action(name, kind):
        return state_action(control, name, kind)
    def wait(name):
        value = action(name, 'Wait')
        result = literal(value['time'])
        if result <= 0 or value['finishEvent'] != 'FINISHED' or value['realTime']:
            raise ValueError(f'Unsupported {name} wait')
        return float(result)
    for name, event, target in (
            ('Idle', 'ATTACK', 'Attack Antic'), ('Attack Antic', 'FINISHED', 'Choice'),
            ('Choice', 'CHASE', 'Zapping'), ('Choice', 'MULTIZAP', 'Pattern Choice'),
            ('Zapping', 'FINISHED', 'Gen'), ('Gen', 'FINISHED', 'First?'),
            ('Gen', 'END', 'Attack Recover'), ('First?', 'FINISHED', 'Zapping'),
            ('Pattern Choice', 'FINISHED', 'Multizap'),
            ('Multizap', 'FINISHED', 'Attack Recover'),
            ('Attack Recover', 'FINISHED', 'Quirrel?')):
        transition = one([t for t in state(name)['transitions']
                          if t['fsmEvent']['name'] == event], f'{name}/{event}')
        if transition['toState'] != target:
            raise ValueError(f'Unsupported attack transition {name}/{event}')
    idle = action('Idle', 'WaitRandom')
    low, high = float(literal(idle['timeMin'])), float(literal(idle['timeMax']))
    if low <= 0 or high < low or idle['finishEvent'] != 'ATTACK' or idle['realTime']:
        raise ValueError('Unsupported Idle wait')
    choice = action('Choice', 'SendRandomEventV2')
    if (choice['events']['elements'] != ['CHASE', 'MULTIZAP'] or
            [v['variable'] for v in choice['trackingInts']['elements']] != ['Ct Chase', 'Ct Multizap']):
        raise ValueError('Unsupported weighted attack interface')
    weights = [float(literal(v)) for v in choice['weights']['elements']]
    limits = [integer(literal(v), 'repeat limit', 1) for v in choice['eventMax']['elements']]
    if len(weights) != 2 or len(limits) != 2 or any(w <= 0 for w in weights):
        raise ValueError('Unsupported weighted attacks')
    reset = action('Choice', 'SetIntValue')
    first = action('Choice', 'SetBoolValue')
    add = action('Gen', 'IntAdd')
    compare = action('Gen', 'IntCompare')
    if (reset['intVariable']['variable'] != 'Zaps' or literal(reset['intValue']) != 0 or reset['everyFrame'] or
            first['boolVariable']['variable'] != 'First Chaser' or
            boolean(literal(first['boolValue']), 'First Chaser') or first['everyFrame'] or
            add['intVariable']['variable'] != 'Zaps' or literal(add['add']) != 1 or add['everyFrame'] or
            compare['integer1']['variable'] != 'Zaps' or compare['equal'] or compare['lessThan'] or
            compare['greaterThan'] != 'END' or compare['everyFrame']):
        raise ValueError('Unsupported chaser count/order')
    enabled = [a['type'].split(',')[0].split('.')[-1] for a in state('Gen')['actions'] if a['enabled']]
    if enabled != ['IntAdd', 'IntCompare', 'SpawnObjectFromGlobalPool']:
        raise ValueError('Gen must compare before spawning (no ninth zap)')
    spawn = action('Gen', 'SpawnObjectFromGlobalPool')
    if (spawn['spawnPoint']['name'] != 'Hero' or spawn['storeObject']['name'] != 'Zap' or
            not spawn['position']['useVariable'] or not spawn['rotation']['useVariable']):
        raise ValueError('Unsupported chaser spawn placement')
    # A synchronous BoolTest event exits before the second SetFsmFloat and Wait.
    first_actions = [a for a in state('First?')['actions'] if a['enabled']]
    if [a['type'].split(',')[0].split('.')[-1] for a in first_actions] != [
            'SetFsmFloat', 'BoolTest', 'SetBoolValue', 'SetFsmFloat', 'Wait']:
        raise ValueError('Unsupported First? action order')
    anticipation = []
    for index in (0, 3):
        params = first_actions[index]['parameters']
        if (params['gameObject']['gameObject']['name'] != 'Zap' or
                params['fsmName']['value'] != 'Control' or params['variableName']['value'] != 'Antic Time' or
                params['everyFrame']):
            raise ValueError('Unsupported chaser anticipation interface')
        anticipation.append(float(literal(params['setValue'])))
    test, flag = [first_actions[i]['parameters'] for i in (1, 2)]
    if (test['boolVariable']['variable'] != 'First Chaser' or test['isTrue'] != 'FINISHED' or
            test['isFalse'] or test['everyFrame'] or flag['boolVariable']['variable'] != 'First Chaser' or
            not boolean(literal(flag['boolValue']), 'First Chaser') or flag['everyFrame']):
        raise ValueError('Unsupported first-chaser branch')
    pattern = action('Pattern Choice', 'GetRandomChild')
    activate = action('Multizap', 'ActivateAllChildren')
    if (pattern['gameObject']['gameObject']['name'] != 'Multizaps' or
            pattern['storeResult']['name'] != 'Current Pattern' or
            activate['gameObject']['name'] != 'Current Pattern' or
            not boolean(activate['activate'], 'activate multizap')):
        raise ValueError('Unsupported multizap activation')
    return dict(idleMin=low, idleMax=high, anticWait=wait('Attack Antic'),
                zapWait=wait('Zapping'), firstZapWait=wait('First?'),
                multizapWait=wait('Multizap'), recoverWait=wait('Attack Recover'),
                zapCount=integer(literal(compare['integer2']), 'chaser count', 1),
                firstAnticipation=anticipation[1], laterAnticipation=anticipation[0],
                chaseWeight=weights[0], multizapWeight=weights[1],
                chaseMax=limits[0], multizapMax=limits[1])


def header(facts):
    rows = ['// Generated by tools/convert_uumuu_attacks.py; copied build22529139 level229.',
            '#pragma once', 'namespace hk3ds::uumuu_attack_data {']
    for name, value in facts.items():
        kind, text = ('unsigned', str(value)) if type(value) is int else ('float', f'{value:.9f}f')
        rows.append(f'inline constexpr {kind} {name}={text};')
    return '\n'.join(rows + ['}', ''])


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=root/'_private/minimum-bosses/uumuu/level229-source.json')
    parser.add_argument('--output', type=Path, default=root/'include/uumuu_attack_data.hpp')
    args = parser.parse_args()
    facts = convert(json.loads(args.source.read_text()))
    args.output.write_text(header(facts))
    print(json.dumps(facts))


if __name__ == '__main__':
    main()
