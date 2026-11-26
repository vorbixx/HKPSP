"""Strict source-data lookups for Watcher encounter conversion.

This module deliberately does not infer arena victory from journal fields.
HealthManager.Die decrements Battle Control/Battle Enemies once for a real
death; the authored battle FSM still owns its victory transition.
"""


def component_data(component):
    data = component['data']
    return data.get('m_Structure', data)


def find_components(actor, kind):
    return [c for c in actor['components'] if c['type'] == kind]


def state_action(fsm, state_name, action_type):
    """Require one enabled authored action; never supply fallback tuning."""
    states = [s for s in fsm['states'] if s['name'] == state_name]
    if len(states) != 1:
        raise ValueError(f'Expected exactly one state {state_name!r}')
    actions = [a for a in states[0]['actions'] if a['enabled'] and
               a['type'].split(',')[0].split('.')[-1] == action_type]
    if len(actions) != 1:
        raise ValueError(f'Expected exactly one {action_type} in {state_name!r}')
    return actions[0]['parameters']
