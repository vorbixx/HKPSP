"""Opt-in source-backed Town Jiji key-door rules; no geometry or save allocation."""
import argparse
import json
import math
from pathlib import Path
import struct

from assetripper_client import ROOT
from inspect_fsm import decode_fsm
from private_output_paths import is_private_output

FORMAT = struct.Struct('<4s12fi64s64s')
SOURCE = 'research/crossroads-scenes/Town/terrain/level7-4067.json'
EDGES = {
    'Idle': [('CONVO START', 'Hero Anim')],
    'Init': [('FINISHED', 'Idle'), ('INACTIVE', 'Inactive')],
    'End': [], 'Box Down': [('FINISHED', 'End')],
    'Box Up': [('FINISHED', 'No Key')], 'Anim End': [('FINISHED', 'Box Down')],
    'Talk Finish': [], 'No Key': [('CONVO_FINISH', 'Talk Finish')],
    'Hero Anim': [('FINISHED', 'Key?')],
    'Key?': [('KEY', 'Box Up YN'), ('NOKEY', 'Box Up')],
    'Box Up YN': [('FINISHED', 'Send Text')],
    'Send Text': [('NO', 'Decline Pause'), ('YES', 'Yes')],
    'Yes': [('FINISHED', 'Activate')],
    'Decline Pause': [('FINISHED', '')], 'Activate': [('FINISHED', 'Tween Antic')],
    'Pause': [('FINISHED', 'Init')], 'Inactive': [],
    'Tween Antic': [('FINISHED', 'Slam')], 'Slam': [('FINISHED', 'Slam End')],
    'Activate Door': [], 'Slam End': [('FINISHED', 'Activate Door')],
}
WAITS = {'Box Up': ('boxUpWait', .3, 'FINISHED'),
         'Box Up YN': ('choiceUpWait', .25, 'FINISHED'),
         'Decline Pause': ('declineWait', .3, 'TALK FINISH'),
         'Box Down': ('boxDownWait', .3, 'FINISHED'),
         'Yes': ('yesWait', .5, 'FINISHED'),
         'Activate': ('activateWait', .5, 'FINISHED'),
         'Tween Antic': ('anticWait', 1.2, 'FINISHED'),
         'Slam End': ('slamEndWait', 1., 'FINISHED')}


def require(ok, message):
    if not ok:
        raise ValueError(message)


def literal(value):
    require(isinstance(value, dict) and value.get('useVariable') is False and
            'variable' not in value and 'value' in value, 'Expected source literal')
    return value['value']


def source_float(value):
    return struct.unpack('<f', struct.pack('<f', value))[0]


def room_data(source):
    require(source['m_GameObject'] == {'m_FileID': 0, 'm_PathID': 818}, 'Changed Jiji door owner')
    fsm = decode_fsm(source['m_Structure']['fsm'])
    require(fsm['name'] == 'Conversation Control' and fsm['start'] == 'Pause',
            'Changed Jiji door FSM')
    states = {s['name']: s for s in fsm['states']}
    require(len(states) == len(fsm['states']) and states.keys() == EDGES.keys(),
            'Changed Jiji door states')
    for name, edges in EDGES.items():
        require([(t['fsmEvent']['name'], t['toState']) for t in states[name]['transitions']] == edges,
                'Changed Jiji door edges: ' + name)

    def action(state, kind):
        actions = [a['parameters'] for a in states[state]['actions']
                   if a['enabled'] and a['type'].split('.')[-1] == kind]
        require(len(actions) == 1, 'Changed Jiji action: ' + state + '/' + kind)
        return actions[0]

    def wait(state, expected, event):
        parameters = action(state, 'Wait')
        require(parameters['realTime'] is False and parameters['finishEvent'] == event and
                literal(parameters['time']) == source_float(expected), 'Changed Jiji wait: ' + state)
        return expected

    int_vars = fsm['variables']['intVariables']
    variables = {v['name']: v['value'] for v in int_vars}
    require(len(int_vars) == len(variables) and variables.get('Decrement') == -1 and
            variables.get('Simple Keys') == 0, 'Changed Jiji integer variables')
    times = [wait(state, value, event) for state, (_, value, event) in WAITS.items()]

    compare = action('Key?', 'IntCompare')
    require(compare == dict(integer1={'variable': 'Simple Keys'},
                            integer2={'value': 0, 'useVariable': False},
                            equal='NOKEY', lessThan='NOKEY', greaterThan='KEY',
                            everyFrame=False), 'Changed Jiji key comparison')
    read = action('Key?', 'GetPlayerDataInt')
    require(literal(read['intName']) == 'simpleKeys' and
            read['storeValue'] == {'variable': 'Simple Keys'}, 'Changed Jiji key read')
    decrement = action('Activate', 'PlayerDataIntAdd')
    require(literal(decrement['intName']) == literal(read['intName']) and
            decrement['amount'] == {'variable': 'Decrement'}, 'Changed Jiji key decrement')
    read_unlock = action('Init', 'PlayerDataBoolTest')
    write_unlock = action('Activate', 'SetPlayerDataBool')
    unlock = literal(read_unlock['boolName'])
    require(unlock == literal(write_unlock['boolName']) == 'jijiDoorUnlocked' and
            read_unlock['isTrue'] == 'INACTIVE' and read_unlock['isFalse'] == '' and
            literal(write_unlock['value']) is True, 'Changed Jiji unlock save flag')

    def tween(state, duration, delta):
        move = action(state, 'iTweenMoveBy')
        require(move['vector'] == {'value': {'x': 0., 'y': source_float(delta), 'z': 0.},
                                   'useVariable': False} and
                literal(move['time']) == source_float(duration) and
                literal(move['realTime']) is False,
                'Changed Jiji tween: ' + state)

    tween('Tween Antic', 1., -1.)
    tween('Slam', .5, -3.1)
    data = dict(waits=times, key='simpleKeys', unlock='jijiDoorUnlocked', decrement=-1,
                anticTweenTime=1., anticDeltaY=-1., slamTweenTime=.5, slamDeltaY=-3.1)
    room_bytes(data)
    return data


def room_bytes(data):
    names = ('boxUpWait', 'choiceUpWait', 'declineWait', 'boxDownWait', 'yesWait',
             'activateWait', 'anticWait', 'slamEndWait', 'anticTweenTime', 'anticDeltaY',
             'slamTweenTime', 'slamDeltaY')
    values = data['waits'] + [data[n] for n in names[8:]]
    require(len(data['waits']) == 8 and all(math.isfinite(v) and abs(v) <= 100000
            for v in values) and all(v >= 0 for v in values[:9:]), 'Invalid Jiji numeric rules')
    require(values[9] < 0 and values[10] >= 0 and values[11] < 0, 'Invalid Jiji tween rules')
    decrement = data['decrement']
    require(isinstance(decrement, int) and decrement < 0, 'Invalid Jiji key decrement')
    def name_bytes(value):
        require(isinstance(value, str), 'Invalid Jiji save key')
        encoded = value.encode('ascii')
        require(0 < len(encoded) < 64 and all(32 <= c <= 126 for c in encoded),
                'Invalid Jiji save key')
        return encoded
    return FORMAT.pack(b'JJD1', *values, decrement,
                       name_bytes(data['key']), name_bytes(data['unlock']))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / '_private' / SOURCE)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    require(is_private_output(args.output), 'Jiji door output must remain private')
    raw = room_bytes(room_data(json.loads(args.source.read_text(encoding='utf-8'))))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(raw)


if __name__ == '__main__':
    main()
