"""Compile source WAKE/BATTLE SPAWN states without inventing wave membership."""
from inspect_fsm import decode_fsm


def value(parameter):
    if isinstance(parameter, dict):
        if parameter.get('useVariable'):
            return parameter.get('name')
        return parameter.get('value', parameter.get('variable'))
    return parameter


def export(fsm, spawns, read):
    decoded = decode_fsm(fsm)
    states = {s['name']: s for s in decoded['states']}
    refs = {v['name']: v['value']['m_PathID'] for v in fsm['variables']['gameObjectVariables']}
    actors = {s['owner']: s for s in spawns}

    def owner(default):
        if not isinstance(default, dict):
            return 0
        ref = default.get('gameObject', default)
        name = value(ref)
        return refs.get(name, 0) if isinstance(name, str) else (name or {}).get('m_PathID', 0)

    for state in states.values():
        for action in state['actions']:
            if not action['enabled']:
                continue
            p = action['parameters']
            if action['type'].split('.')[-1] == 'FindGameObject':
                matches = [s['owner'] for s in spawns if s['prefab'] == value(p['objectName'])]
                if len(matches) == 1:
                    refs[value(p['store'])] = matches[0]

    waves = {}
    for name, state in states.items():
        members = []; positions = {}; hpvars = {}; health = 0; below = 0; timeout = 0
        for action in state['actions']:
            if not action['enabled']:
                continue
            p = action['parameters']; kind = action['type'].split('.')[-1]
            if kind == 'SetPosition':
                target = owner(p['gameObject'])
                x, y = value(p['x']), value(p['y'])
                if target in actors and isinstance(x, (int, float)) and isinstance(y, (int, float)):
                    positions[target] = [x, y]
                    if target not in members:
                        members.append(target)
            if kind in ('SendEvent', 'SendEventByName') and value(p.get('sendEvent')) in ('WAKE', 'BATTLE SPAWN'):
                target = owner(p.get('eventTarget', {}).get('gameObject', {}))
                if target in actors and target not in members:
                    members.append(target)
            if kind in ('GetHP', 'GetHPEveryFrame'):
                hpvars[value(p['storeValue'])] = owner(p['target'])
            if kind == 'IntCompare' and value(p.get('integer1')) in hpvars and value(p.get('lessThan')):
                health = hpvars[value(p['integer1'])]; below = value(p['integer2'])
            if kind == 'Wait':
                timeout = value(p['time'])
        if members:
            waves[name] = dict(spawns=[dict(owner=o, position=positions.get(o, actors[o]['position']),
                                           reposition=o in positions) for o in members],
                               delay=0, timeout=timeout, health_owner=health, health_below=below)
    if len(waves) < 2:
        return []

    def following(name):
        queue = [(t['toState'], 0) for t in states[name]['transitions']]; seen = {name}
        while queue:
            target, delay = queue.pop(0)
            if target in seen or target not in states:
                continue
            if target in waves:
                return target, delay
            seen.add(target)
            for a in states[target]['actions']:
                if a['enabled'] and a['type'].split('.')[-1] == 'Wait':
                    delay += value(a['parameters']['time'])
            queue.extend((t['toState'], delay) for t in states[target]['transitions'])
        return None

    first = following('Detect') if 'Detect' in states else None
    if not first:
        return []
    result = []; name, delay = first; seen = set()
    while name not in seen:
        seen.add(name); wave = waves[name]; wave['delay'] = delay; result.append(wave)
        next_wave = following(name)
        if not next_wave:
            break
        name, delay = next_wave
    if len(result) != len(waves):
        raise ValueError('Unsupported branching battle waves: ' + ', '.join(waves))
    return result
