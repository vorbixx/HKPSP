"""Decode observed PlayMaker v1/v2 parameter layouts, never execute them.

Unsupported types remain explicit raw records. Output contains original data and
must stay private. See docs/reverse-engineering.md for provenance and limits.
"""
import argparse
import json
import struct
from pathlib import Path

POOLS = {5: 'unityObjectParams', 18: 'fsmStringParams', 19: 'fsmGameObjectParams',
         20: 'fsmOwnerDefaultParams', 21: 'functionCallParams',
         22: 'animationCurveParams', 24: 'fsmObjectParams', 31: 'fsmEventTargetParams',
         32: 'fsmObjectParams',
         36: 'fsmPropertyParams', 39: 'fsmVarParams', 41: 'fsmArrayParams', 42: 'fsmEnumParams'}

# FsmUtility.ByteArrayToFsm*: components, UseVariable byte, UTF-8 name.
V1_COMPONENTS = {37: 'xy', 28: 'xyz', 25: 'rgba', 35: 'xyzw'}


def decode_param(data, i, version):
    """Decode one record; Array descriptors own the following N records.

    Array elements also remain in decode_fsm's flat ordered_parameters view.
    This preserves record order and the historical last-name-wins dictionary.
    """
    return _decode_param(data, i, version, len(data['paramDataType']))


def _decode_param(data, i, version, end):
    kind, pos, size = (data[key][i] for key in ('paramDataType', 'paramDataPos', 'paramByteDataSize'))
    if kind == 12:
        # ActionData.LoadActionField/LoadArrayElement: pos indexes descriptors,
        # not byteData, even when paramByteDataSize is zero.
        if not 0 <= pos < min(len(data['arrayParamSizes']), len(data['arrayParamTypes'])):
            raise ValueError(f'Array record {i}: descriptor index {pos} out of bounds')
        count = data['arrayParamSizes'][pos]
        if count < 0 or i + 1 + count > end:
            raise ValueError(f'Array record {i}: {count} elements cross the action boundary {end}')
        return {'element_type': data['arrayParamTypes'][pos],
                'elements': [_decode_param(data, j, version, end)
                             for j in range(i + 1, i + 1 + count)]}
    if kind in POOLS:
        return data[POOLS[kind]][pos]
    if version == 2:
        pools = {15:'fsmFloatParams',16:'fsmIntParams',17:'fsmBoolParams',
                 37:'fsmVector2Params',28:'fsmVector3Params',25:'fsmColorParams',
                 30:'fsmRectParams',35:'fsmQuaternionParams',3:'stringParams',23:'stringParams'}
        if kind in pools:
            return data[pools[kind]][pos]
    raw = bytes(data['byteData'][pos:pos + size])
    if version == 1:
        if kind in V1_COMPONENTS:
            components = V1_COMPONENTS[kind]
            width = 4 * len(components)
            if pos < 0 or size < width + 1 or pos + size > len(data['byteData']):
                raise ValueError(f'Parameter record {i}: truncated v1 component value')
            name = raw[width + 1:].decode('utf-8')
            if name:
                return {'variable': name}
            values = struct.unpack_from('<' + 'f' * len(components), raw)
            return {'value': dict(zip(components, values)), 'useVariable': bool(raw[width])}
        if kind in (15, 16, 17):
            width, fmt = (1, '?') if kind == 17 else (4, 'f' if kind == 15 else 'i')
            name = raw[width + 1:].decode('utf-8')
            return {'variable': name} if name else {'value': struct.unpack_from('<' + fmt, raw)[0], 'useVariable': bool(raw[width])}
        if kind in (3, 23):
            return raw.decode('utf-8')
    if version in (1,2):
        if kind in (0, 7) and len(raw)==4: return struct.unpack('<i', raw)[0]
        if kind == 1: return bool(raw[0])
        if kind == 2: return struct.unpack('<f', raw)[0]
    return {'unsupported_type': kind, 'position': pos, 'bytes': list(raw)}


def decode_fsm(fsm):
    """Expose authored metadata and records without executing variable lookups."""
    states = []
    for state in fsm['states']:
        data = state['actionData']
        actions = []
        starts = data['actionStartIndex'] + [len(data['paramName'])]
        if (len(starts) != len(data['actionNames']) + 1 or starts != sorted(starts)
                or (data['actionNames'] and starts[0] != 0) or any(n < 0 for n in starts)):
            raise ValueError(f"State {state['name']}: invalid action parameter boundaries")
        for a, name in enumerate(data['actionNames']):
            ordered = [{'name': data['paramName'][i],
                        'value': _decode_param(data, i, fsm['dataVersion'], starts[a + 1])}
                       for i in range(starts[a], starts[a + 1])]
            action = {'type': name, 'enabled': data['actionEnabled'][a],
                      'ordered_parameters': ordered,
                      'parameters': {p['name']: p['value'] for p in ordered}}
            for source, target in (('customNames', 'custom_name'), ('actionIsOpen', 'is_open'),
                                   ('actionHashCodes', 'hash_code')):
                if source in data:
                    action[target] = data[source][a]
            actions.append(action)
        states.append({**{k: v for k, v in state.items() if k != 'actionData'}, 'actions': actions})
    return {**{k: v for k, v in fsm.items() if k not in ('startState', 'states')},
            'start': fsm['startState'], 'states': states}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    private = Path(__file__).resolve().parents[1] / '_private'
    if not args.output.resolve().is_relative_to(private):
        parser.error('Decoded FSM output must be inside _private')
    result = decode_fsm(json.loads(args.input.read_text(encoding='utf-8-sig'))['m_Structure']['fsm'])
    args.output.write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding='utf-8')
