"""Constant, one-shot visual translations in the initial PlayMaker state.

Only unconditional world-space Translate actions on the FSM owner are baked.
Event-driven motion and variable-dependent transforms remain runtime work.
"""
import math

from inspect_fsm import decode_param
from unity_transform import project


def project_with_startup(point, chain, offsets, source_chain=None):
    """Apply world offsets using source identities even when animation copies a transform."""
    result = project(point, chain)
    sources = chain if source_chain is None else source_chain
    return tuple(value + sum(offsets.get(id(t), (0., 0., 0.))[axis] for t in sources)
                 for axis, value in enumerate(result))
from unity_transform import project


def project_with_startup(point, chain, offsets, source_chain=None):
    """Apply world offsets using source identities even when animation copies a transform."""
    result = project(point, chain)
    sources = chain if source_chain is None else source_chain
    return tuple(value + sum(offsets.get(id(t), (0., 0., 0.))[axis] for t in sources)
                 for axis, value in enumerate(result))


def world_translations(mapping, read):
    offsets = {}
    seen = set()
    owners = {}
    for item in mapping['inventory']:
        if not item['active']:
            continue
        owners[item['chain'][0]] = item.get('components', {})
        for owner, transform in zip(item.get('owners', ()), item['chain']):
            if owner in mapping.get('owner_components', {}):
                owners[transform] = mapping['owner_components'][owner]
    for transform, components in owners.items():
        for key in components.get('PlayMakerFSM', ()):
            if key in seen:
                continue
            seen.add(key)
            component = read(key)
            if not component.get('m_Enabled', 1):
                continue
            fsm = component['m_Structure']['fsm']
            state = next((s for s in fsm['states'] if s['name'] == fsm['startState']), None)
            if state is None:
                continue
            data = state['actionData']
            # Only the unconditional prefix is safe: a later action could leave
            # the state, but cannot undo a translation that has already run.
            actions = [i for i, enabled in enumerate(data['actionEnabled']) if enabled]
            starts = data['actionStartIndex'] + [len(data['paramName'])]
            for i in actions:
                if data['actionNames'][i] != 'HutongGames.PlayMaker.Actions.Translate':
                    break
                params = {data['paramName'][j]: decode_param(data, j, fsm['dataVersion'])
                          for j in range(starts[i], starts[i+1])}
                owner = params['gameObject']
                vector = params['vector']
                if (owner['ownerOption'] != 0 or params['space'] != 0
                        or params['perSecond'] or params['everyFrame']
                        or not vector.get('useVariable') or vector.get('name')):
                    continue
                delta = []
                for axis in 'xyz':
                    value = params[axis]
                    if 'variable' in value or 'value' not in value:
                        break
                    if value.get('useVariable'):
                        if value.get('name'):
                            break
                        delta.append(0.)  # PlayMaker's unassigned/None FsmFloat.
                    else:
                        delta.append(value['value'])
                if len(delta) != 3:
                    continue
                if not all(math.isfinite(v) for v in delta):
                    raise ValueError('Non-finite startup visual translation')
                previous = offsets.get(transform, (0., 0., 0.))
                offsets[transform] = tuple(a+b for a, b in zip(previous, delta))
    return offsets
