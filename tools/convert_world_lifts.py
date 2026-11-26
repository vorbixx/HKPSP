"""Opt-in private Town Mine Exit Lift rules; no geometry or save allocation."""
import argparse
import json
import math
from pathlib import Path
import struct

from assetripper_client import ROOT
from inspect_fsm import decode_fsm
from private_output_paths import is_private_output

FORMAT = struct.Struct('<4s5f64s')
SOURCE = 'research/crossroads-scenes/Town/terrain/level7-4026.json'
EDGES = {
    'Init': [('START UP', 'Start Up'), ('START DOWN', 'Start Down')],
    'Start Up': [('FINISHED', 'Up')], 'Up': [('TOUCHED', 'Bob')],
    'Start Down': [('FINISHED', 'Down')], 'Down': [('TOUCHED', 'Bob')],
    'Bob': [('FINISHED', 'Up or Down?')],
    'Up or Down?': [('MOVE UP', 'Move Up'), ('MOVE DOWN', 'Move Down')],
    'Move Up': [('DEST REACHED', 'Reached Top')],
    'Move Down': [('DEST REACHED', 'Reached Bot'), ('SEND COLLIDE', 'Move Up')],
    'Reached Top': [('FINISHED', 'Up')], 'Reached Bot': [('FINISHED', 'Down')],
    'Pause': [('FINISHED', 'Check Active')],
    'Check Active': [('ACTIVE', 'Activate'), ('INACTIVE', 'Inactive')],
    'Inactive': [('OPEN', 'Open')], 'Open': [('FINISHED', 'Init')],
    'Activate': [('FINISHED', 'Init')],
}


def require(ok, message):
    if not ok:
        raise ValueError(message)


def literal(value):
    require(isinstance(value, dict) and value.get('useVariable') is False and
            'variable' not in value and 'value' in value, 'Expected source literal')
    return value['value']


def room_data(source):
    require(source['m_GameObject'] == {'m_FileID': 0, 'm_PathID': 703}, 'Changed lift owner')
    fsm = decode_fsm(source['m_Structure']['fsm'])
    require(fsm['name'] == 'Lift Control' and fsm['start'] == 'Pause', 'Changed lift FSM')
    states = {s['name']: s for s in fsm['states']}
    require(len(states) == len(fsm['states']) and states.keys() == EDGES.keys(), 'Changed lift states')
    for name, edges in EDGES.items():
        require([(t['fsmEvent']['name'], t['toState']) for t in states[name]['transitions']] == edges,
                'Changed lift edges: ' + name)

    def action(state, kind):
        actions = [a['parameters'] for a in states[state]['actions']
                   if a['enabled'] and a['type'].split('.')[-1] == kind]
        require(len(actions) == 1, 'Changed lift action: ' + state + '/' + kind)
        return actions[0]

    values = fsm['variables']['floatVariables']
    variables = {v['name']: v['value'] for v in values}
    require(len(values) == len(variables), 'Duplicate lift variable')
    rules = [variables[n] for n in ('Bot Start Y', 'Top Start Y', 'Speed')]
    for state in ('Bob', 'Reached Top', 'Reached Bot'):
        wait = action(state, 'Wait')
        require(wait['realTime'] is False and wait['finishEvent'] == 'FINISHED', 'Unscaled lift wait')
        rules.append(literal(wait['time']))
    require(rules[-1] == rules[-2], 'Unequal arrival waits')
    rules.pop()
    for state, variable in (('Move Up', 'Speed'), ('Move Down', 'Speed Down')):
        move = action(state, 'Translate')
        require(move['gameObject']['ownerOption'] == 0 and move['y'] == {'variable': variable} and
                move['space'] == 1 and move['perSecond'] is True and move['everyFrame'] is True and
                move['lateUpdate'] is False and move['fixedUpdate'] is False and
                all(move[c] == {'value': 0.0, 'useVariable': True} for c in ('x', 'z')) and
                move['vector'] == {'value': {'x': 0.0, 'y': 0.0, 'z': 0.0}, 'useVariable': True},
                'Unsupported lift movement')
    init = [a['parameters'] for a in states['Init']['actions'] if a['enabled'] and
            a['type'].split('.')[-1] in ('SetFloatValue', 'FloatMultiply', 'FloatSubtract', 'FloatAdd')]
    require(init == [
        {'floatVariable': {'variable': 'Speed Down'}, 'floatValue': {'variable': 'Speed'}, 'everyFrame': False},
        {'floatVariable': {'variable': 'Speed Down'}, 'multiplyBy': {'value': -1.0, 'useVariable': False}, 'everyFrame': False},
        {'floatVariable': {'variable': 'Hero Mid Y'}, 'floatValue': {'variable': 'Top Start Y'}, 'everyFrame': False},
        {'floatVariable': {'variable': 'Hero Mid Y'}, 'subtract': {'variable': 'Bot Start Y'}, 'everyFrame': False, 'perSecond': False},
        {'floatVariable': {'variable': 'Hero Mid Y'}, 'multiplyBy': {'value': 0.5, 'useVariable': False}, 'everyFrame': False},
        {'floatVariable': {'variable': 'Hero Mid Y'}, 'add': {'variable': 'Bot Start Y'}, 'everyFrame': False, 'perSecond': False},
    ], 'Changed lift speed/midpoint arithmetic')
    for state, left, right, equal, less, greater, every in (
        ('Init', 'Hero Y', 'Hero Mid Y', '', 'START DOWN', 'START UP', False),
        ('Up', 'Hero Y', 'Hero Mid Y', '', 'TOUCHED', '', True),
        ('Down', 'Hero Y', 'Hero Mid Y', '', '', 'TOUCHED', True),
        ('Move Up', 'Self Y', 'Top Start Y', 'DEST REACHED', '', 'DEST REACHED', True),
        ('Move Down', 'Self Y', 'Bot Start Y', 'DEST REACHED', 'DEST REACHED', '', True),
    ):
        compare = action(state, 'FloatCompare')
        require(compare == dict(float1={'variable': left}, float2={'variable': right},
                tolerance={'value': 0.0, 'useVariable': False}, equal=equal,
                lessThan=less, greaterThan=greater, everyFrame=every), 'Changed lift comparison')
    for state, endpoint in (('Start Up', 'Top Start Y'), ('Start Down', 'Bot Start Y'),
                            ('Reached Top', 'Top Start Y'), ('Reached Bot', 'Bot Start Y')):
        position = action(state, 'SetPosition')
        require(position['y'] == {'variable': endpoint} and not position['everyFrame'] and
                not position['lateUpdate'], 'Changed lift endpoint')
    read = action('Check Active', 'PlayerDataBoolTest')
    write = action('Open', 'SetPlayerDataBool')
    key = literal(read['boolName'])
    require(key == literal(write['boolName']) and literal(write['value']) is True and
            read['isTrue'] == 'ACTIVE' and read['isFalse'] == 'INACTIVE', 'Changed lift save key')
    data = dict(rules=rules, key=key)
    room_bytes(data)  # Validate the entire binary contract before returning.
    return data


def room_bytes(data):
    rules = data['rules']
    require(len(rules) == 5 and all(math.isfinite(v) and abs(v) <= 100000 for v in rules) and
            rules[0] < rules[1] and rules[2] > 0 and rules[3] >= 0 and rules[4] >= 0,
            'Unsupported lift numeric rules')
    key = data['key'].encode('ascii')
    require(0 < len(key) < 64 and all(32 <= c <= 126 for c in key), 'Invalid lift save key')
    return FORMAT.pack(b'MLF1', *rules, key)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / '_private' / SOURCE)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    require(is_private_output(args.output), 'Lift output must remain private')
    raw = room_bytes(room_data(json.loads(args.source.read_text(encoding='utf-8'))))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(raw)


if __name__ == '__main__':
    main()
