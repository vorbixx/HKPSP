"""Validate copied Uumuu chase/body/bounds actions and emit motion constants."""
import converted_files  # Install copy-on-write protection before output writes.
import argparse
import json
from pathlib import Path

from convert_uumuu import literal, one
from watcher_battle import component_data, find_components, state_action


def require(condition, description):
    if not condition:
        raise ValueError(f'Unsupported Uumuu motion: {description}')


def actions(state):
    return [a for a in state['actions'] if a['enabled']]


def kind(action):
    return action['type'].split(',')[0].split('.')[-1]


def self_owner(value):
    require(value['ownerOption'] == 0, 'action must target owner')


def unused(value):
    # PlayMaker's unnamed variable is the authored "None" field.
    return bool(value.get('useVariable')) and not value.get('variable') and not value.get('name')


def bounds_facts(bounds):
    def state(name):
        return one([s for s in bounds['states'] if s['name'] == name], name)

    def transitions(st):
        return [(t['fsmEvent']['name'], t['toState']) for t in st['transitions']]

    require(bounds['start'] == 'Init' and not bounds['globalTransitions'], 'dormant bounds start')
    require(not actions(state('Init')) and transitions(state('Init')) == [('AWAKE', 'Idle')],
            'bounds must await AWAKE')
    require(set(s['name'] for s in bounds['states']) ==
            {'Init', 'Idle', 'Under X', 'Over X', 'Under Y', 'Over Y'}, 'bounds states')
    idle = state('Idle')
    enabled = actions(idle)
    require([kind(a) for a in enabled] == ['GetPosition'] + ['FloatCompare'] * 4,
            'bounds check action order')
    position = enabled[0]['parameters']
    self_owner(position['gameObject'])
    require(unused(position['vector']) and unused(position['z']) and
            position['x'] == {'variable': 'Self X'} and position['y'] == {'variable': 'Self Y'} and
            position['space'] == 0 and position['everyFrame'], 'bounds world position sample')
    cases = [('X Min', 'Under X', 'x', 5.0), ('X Max', 'Over X', 'x', -5.0),
             ('Y Min', 'Under Y', 'y', 5.0), ('Y Max', 'Over Y', 'y', -5.0)]
    require(sorted(transitions(idle)) == sorted((name.upper(), name) for _, name, _, _ in cases),
            'bounds idle transitions')
    for action, (variable, name, axis, velocity) in zip(enabled[1:], cases):
        p = action['parameters']
        low = velocity > 0
        require(p['float1'] == {'variable': f'Self {axis.upper()}'} and
                p['float2'] == {'variable': variable} and literal(p['tolerance']) == 0 and
                p['equal'] == '' and p['lessThan'] == (name.upper() if low else '') and
                p['greaterThan'] == ('' if low else name.upper()) and p['everyFrame'],
                f'strict ordered {variable} comparison')
        snap = state(name)
        require(transitions(snap) == [('FINISHED', 'Idle')] and
                [kind(a) for a in actions(snap)] == ['SetPosition', 'SetVelocity2d'],
                f'{name} snap order/return')
        pos, vel = [a['parameters'] for a in actions(snap)]
        for p in (pos, vel):
            self_owner(p['gameObject'])
            require(unused(p['vector']) and not p['everyFrame'], f'{name} component-only write')
        require(pos[axis] == {'variable': variable} and pos['space'] == 0 and
                not pos['lateUpdate'] and unused(pos['z']), f'{name} position snap')
        require(literal(vel[axis]) == velocity, f'{name} inward velocity')
        other = 'y' if axis == 'x' else 'x'
        require(unused(pos[other]) and unused(vel[other]), f'{name} preserves other axis')
    values = {name: literal(one([v for v in bounds['variables']['floatVariables']
                                if v['name'] == name], name)['value'])
              for name, _, _, _ in cases}
    require(values == {'X Min': 36.459999084472656, 'X Max': 69.18000030517578,
                       'Y Min': 105.7300033569336, 'Y Max': 131.2100067138672}, 'authored bounds')
    return dict(minX=values['X Min'], maxX=values['X Max'], minY=values['Y Min'],
                maxY=values['Y Max'], inwardSpeed=5.0)


def convert(source):
    require(source['build_id'] == 22529139 and source['scene'] == 'level229', 'copied source identity')
    actor = one([a for a in source['actors'] if a['name'] == 'Mega Jellyfish'], 'Mega Jellyfish')
    body = component_data(one(find_components(actor, 'Rigidbody2D'), 'Uumuu Rigidbody2D'))
    require(body['m_Mass'] == 1.0 and body['m_LinearDamping'] == 0 and
            body['m_GravityScale'] == 0 and body['m_BodyType'] == 0 and
            body['m_Simulated'] and not body['m_UseAutoMass'], 'dynamic mass/damping/gravity')
    bounce = component_data(one(find_components(actor, 'ObjectBounce'), 'Uumuu ObjectBounce'))
    require(bounce['m_Enabled'] == 1 and bounce['bounceFactor'] == 0.5 and
            bounce['speedThreshold'] == 1.0, 'enabled authored bounce tuning')
    require(not bounce['playSound'] and not bounce['playAnimationOnBounce'] and
            not bounce['sendFSMEvent'], 'bounce has no configured audiovisual/FSM side effects')
    recoil = component_data(one(find_components(actor, 'Recoil'), 'Uumuu Recoil'))
    require(recoil['m_Enabled'] == 1 and not recoil['freezeInPlace'] and
            not recoil['preventRecoilUp'] and recoil['stopVelocityXWhenRecoilingUp'],
            'enabled translating recoil configuration')
    require(recoil['recoilSpeedBase'] == 15.0 and
            recoil['recoilDuration'] == 0.15000000596046448, 'authored recoil tuning')

    def fsm(name):
        return one([c['decoded'] for c in actor['components']
                    if c.get('decoded', {}).get('name') == name], name)

    control = fsm('Mega Jellyfish')
    for name, speed in [('Wounded', 15.0), ('Recover', 0.0)]:
        p = state_action(control, name, 'SetRecoilSpeed')
        self_owner(p['target'])
        require(literal(p['newRecoilSpeed']) == speed, f'{name} recoil speed')
    facts = {}
    groups = [('idle', ['Idle'], 6.0, 6.0),
              ('antic', ['Attack Antic', 'Arrive Pause', 'Q Arrive', 'Q Leave',
                         'Slash Pause', 'Attack Pause'], 3.0, 3.0),
              ('wounded', ['Wounded'], 2.0, -1.0)]
    for prefix, states, speed, force in groups:
        for name in states:
            p = state_action(control, name, 'ChaseObjectV2')
            self_owner(p['gameObject'])
            require(p['target'].get('useVariable') and p['target']['name'] == 'Hero',
                    f'{name} hero chase target')
            require(literal(p['speedMax']) == speed and literal(p['accelerationForce']) == force and
                    literal(p['offsetX']) == 0 and literal(p['offsetY']) == 0, f'{name} chase tuning')
        facts[prefix + 'Speed'] = float(literal(p['speedMax']))
        facts[prefix + 'Force'] = float(literal(p['accelerationForce']))
    for name in ['Zapping', 'Multizap', 'Slash Antic', 'Slash', 'Recover Antic']:
        p = state_action(control, name, 'DecelerateV2')
        self_owner(p['gameObject'])
        require(literal(p['deceleration']) == 0.8500000238418579, f'{name} damping')
    return dict(**facts, damping=float(literal(p['deceleration'])), mass=float(body['m_Mass']),
                bounceFactor=float(bounce['bounceFactor']), bounceThreshold=float(bounce['speedThreshold']),
                recoilSpeed=float(recoil['recoilSpeedBase']), recoilDuration=float(recoil['recoilDuration']),
                woundedRecoilSpeed=15.0, guardedRecoilSpeed=0.0,
                **bounds_facts(fsm('Bounds')))


def header(facts):
    rows = ['// Generated by tools/convert_uumuu_motion.py from copied build22529139 level229.',
            '#pragma once', 'namespace hk3ds::uumuu_motion_data {']
    rows += [f'inline constexpr float {name}={value:.9f}f;' for name, value in facts.items()]
    return '\n'.join(rows + ['}', ''])


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=root/'_private/minimum-bosses/uumuu/level229-source.json')
    parser.add_argument('--output', type=Path, default=root/'include/uumuu_motion_data.hpp')
    args = parser.parse_args()
    facts = convert(json.loads(args.source.read_text()))
    args.output.write_text(header(facts))
    print(json.dumps(facts))


if __name__ == '__main__':
    main()
