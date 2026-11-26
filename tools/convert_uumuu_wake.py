"""Validate the saved Mega Jellyfish wake FSM and emit its timing facts."""
import converted_files  # Install copy-on-write protection before output writes.
import argparse
import json
from pathlib import Path

from convert_uumuu import literal, one


def kind(action):
    return action['type'].split(',')[0].split('.')[-1]


def state_named(fsm, name):
    return one([state for state in fsm['states'] if state['name'] == name], name)


def event_value(value):
    if isinstance(value, dict):
        if value.get('useVariable'):
            raise ValueError('Expected literal Uumuu wake event/name')
        return value['value']
    return value


def state_contract(fsm, name, action_kinds, transitions):
    state = state_named(fsm, name)
    actual_actions = [(kind(action), bool(action['enabled'])) for action in state['actions']]
    if action_kinds is not None and actual_actions != action_kinds:
        raise ValueError(f'Unsupported Uumuu wake actions in {name}')
    actual_transitions = [(edge['fsmEvent']['name'], edge['toState']) for edge in state['transitions']]
    if actual_transitions != transitions:
        raise ValueError(f'Unsupported Uumuu wake transitions in {name}')
    return state


def action(state, action_kind, index=None):
    found = [(i, item) for i, item in enumerate(state['actions']) if kind(item) == action_kind]
    if index is not None:
        found = [(i, item) for i, item in found if i == index]
    item = one(found, f'{action_kind} action in {state["name"]}')[1]
    if not item['enabled']:
        raise ValueError(f'Expected enabled {action_kind} in {state["name"]}')
    return item['parameters']


def target_is(game_object, owner_option, name=''):
    return (game_object['ownerOption'] == owner_option and
            game_object['gameObject'].get('name', '') == name)


def component(actor, type_name):
    return one([item for item in actor['components'] if item['type'] == type_name], type_name)['data']


def convert(source):
    if source.get('build_id') != 22529139 or source.get('scene') != 'level229':
        raise ValueError('Expected copied build22529139 level229')
    jelly = one([actor for actor in source['actors'] if actor['name'] == 'Mega Jellyfish'], 'Mega Jellyfish')
    fsm = one([component['decoded'] for component in jelly['components']
               if component.get('decoded', {}).get('name') == 'Mega Jellyfish'], 'Mega Jellyfish FSM')
    if fsm.get('start') != 'Init' or fsm.get('globalTransitions'):
        raise ValueError('Unsupported Uumuu wake FSM start/global transitions')

    expected = {
        'Init': (None, [('FINISHED', 'Sleep')]),
        'Sleep': ([], [('BATTLE START', 'Wake Pause')]),
        'Wake Pause': ([('Wait', True)], [('FINISHED', 'Wake Rumble')]),
        'Wake Rumble': ([('AudioPlaySimple', True), ('SetFsmBool', True),
                         ('PlayParticleEmitter', False), ('PlayParticleEmitter', True),
                         ('PlayParticleEmitter', True), ('Wait', True)],
                        [('FINISHED', 'Burst')]),
        'Burst': ([(name, True) for name in
                   ('AudioStop', 'AudioPlaySimple', 'iTweenMoveBy', 'SetCollider',
                    'SetCircleCollider', 'SetFsmBool', 'SendEventByName',
                    'StopParticleEmitter', 'StopParticleEmitter',
                    'PlayParticleEmitter', 'PlayParticleEmitter')],
                  [('FINISHED', 'Music')]),
        'Music': ([(name, True) for name in
                   ('GGCheckIfBossScene', 'TransitionToAudioSnapshot', 'ApplyMusicCue')],
                  [('FINISHED', 'Start')]),
        'Start': ([(name, True) for name in
                   ('SetAudioClip', 'AudioPlaySimple', 'SetMeshRenderer', 'SetMeshRenderer',
                    'Wait', 'StopParticleEmitter', 'SetGameObject', 'ActivateGameObject',
                    'SetFsmBool', 'SetFsmString')],
                  [('FINISHED', 'Idle')]),
    }
    states = {name: state_contract(fsm, name, *contract) for name, contract in expected.items()}

    def wait_seconds(state_name, index):
        params = action(states[state_name], 'Wait', index)
        duration = literal(params['time'])
        if (duration <= 0 or params['finishEvent'] != 'FINISHED' or params['realTime']):
            raise ValueError(f'Unsupported scaled wait in {state_name}')
        return float(duration)

    wake_pause = wait_seconds('Wake Pause', 0)
    rumble_wait = wait_seconds('Wake Rumble', 5)
    start_wait = wait_seconds('Start', 4)
    if (wake_pause, rumble_wait, start_wait) != (1.0, 3.0, 1.0):
        raise ValueError('Unsupported Uumuu wake wait durations')

    rumble = action(states['Wake Rumble'], 'SetFsmBool', 1)
    if (event_value(rumble['fsmName']) != 'CameraShake' or
            event_value(rumble['variableName']) != 'RumblingMed' or
            literal(rumble['setValue']) is not True or
            not target_is(rumble['gameObject'], 1, 'CameraParent')):
        raise ValueError('Unsupported Uumuu rumble start')

    tween = action(states['Burst'], 'iTweenMoveBy', 2)
    vector = tween['vector']['value']
    delta_x = literal(vector['x'])
    delta_y = literal(vector['y'])
    if (not target_is(tween['gameObject'], 0) or literal(vector['z']) != 0 or
            (delta_x, delta_y) != (0, 23) or literal(tween['time']) != 1 or
            literal(tween['delay']) != 0 or tween['easeType'] != 4 or
            tween['space'] != 0 or tween['loopType'] != 0 or
            literal(tween['realTime']) is not False or
            literal(tween['stopOnExit']) is not True or tween['finishEvent'] != 'FINISHED'):
        raise ValueError('Unsupported Uumuu burst tween')
    for index, name in ((3, 'SetCollider'), (4, 'SetCircleCollider')):
        collider = action(states['Burst'], name, index)
        if (not target_is(collider['gameObject'], 0) or
                literal(collider['active']) is not True):
            raise ValueError(f'Unsupported Uumuu burst {name}')

    music_check = action(states['Music'], 'GGCheckIfBossScene', 0)
    if (event_value(music_check['bossSceneEvent']) != 'FINISHED' or
            event_value(music_check['regularSceneEvent']) != ''):
        raise ValueError('Unsupported Uumuu music branch events')
    music_snapshot = action(states['Music'], 'TransitionToAudioSnapshot', 1)
    music_cue = action(states['Music'], 'ApplyMusicCue', 2)
    if (literal(music_snapshot['transitionTime']) != 0 or
            literal(music_cue['delayTime']) != 0 or literal(music_cue['transitionTime']) != 0):
        raise ValueError('Unsupported Uumuu music request timing')

    renderer_actions = [item for item in states['Start']['actions'] if kind(item) == 'SetMeshRenderer']
    owner_renderer, entry_renderer = [item['parameters'] for item in renderer_actions]
    if (not target_is(owner_renderer['gameObject'], 0) or
            literal(owner_renderer['active']) is not True or
            not target_is(entry_renderer['gameObject'], 1, 'Entry Sprite') or
            literal(entry_renderer['active']) is not False):
        raise ValueError('Unsupported Uumuu wake renderer actions')

    mesh = component(jelly, 'MeshRenderer')
    box = component(jelly, 'BoxCollider2D')
    circle = component(jelly, 'CircleCollider2D')
    initial_mesh = bool(mesh['m_Enabled'])
    initial_box = bool(box['m_Enabled'])
    initial_circle = bool(circle['m_Enabled'])
    if initial_mesh or initial_box or initial_circle or not box['m_IsTrigger'] or not circle['m_IsTrigger']:
        raise ValueError('Unsupported initial Uumuu renderer/collider state')

    return dict(wakePause=wake_pause, rumbleWait=rumble_wait,
                burstDuration=float(literal(tween['time'])), burstDeltaX=delta_x,
                burstDeltaY=delta_y, startWait=start_wait,
                burstEaseType=int(tween['easeType']), burstSpace=int(tween['space']),
                burstLoopType=int(tween['loopType']), initialMeshEnabled=initial_mesh,
                initialBoxColliderEnabled=initial_box,
                initialCircleColliderEnabled=initial_circle)


def header(facts):
    rows = ['// Generated by tools/convert_uumuu_wake.py from copied build22529139 level229.',
            '#pragma once', 'namespace hk3ds::uumuu_wake_data {']
    for name in ('wakePause', 'rumbleWait', 'burstDuration', 'burstDeltaX', 'burstDeltaY', 'startWait'):
        rows.append(f'inline constexpr float {name}={facts[name]:.9f}f;')
    for name in ('burstEaseType', 'burstSpace', 'burstLoopType'):
        rows.append(f'inline constexpr int {name}={facts[name]};')
    for name in ('initialMeshEnabled', 'initialBoxColliderEnabled', 'initialCircleColliderEnabled'):
        rows.append(f'inline constexpr bool {name}={str(facts[name]).lower()};')
    return '\n'.join(rows + ['}', ''])


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=root/'_private/minimum-bosses/uumuu/level229-source.json')
    parser.add_argument('--output', type=Path, default=root/'include/uumuu_wake_data.hpp')
    args = parser.parse_args()
    facts = convert(json.loads(args.source.read_text(encoding='utf-8')))
    args.output.write_text(header(facts), encoding='utf-8')
    print(json.dumps(facts))


if __name__ == '__main__':
    main()
