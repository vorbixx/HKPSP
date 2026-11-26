"""Source room transforms for the researched Crossroads_03 two-gate switch.

The normal-material presentation is provisional until its external material and
original-player frame are admitted. All authored tint/transform values survive.
"""
import math
import struct

from convert_room_visuals import project
from inspect_fsm import decode_fsm


def room_data(read, library, sprite_ids):
    remap = {sid: index for index, sid in enumerate(sprite_ids)}
    parts = []
    for owner, name in ((1180, 'Toll Gate Switch'), (809, 'Toll Gate'), (611, 'Toll Gate 1')):
        go = read(f'level39-{owner}.json')
        if go['m_Name'] != name or not go['m_IsActive']:
            raise ValueError('Unexpected switch/gate owner')
        components = [(ref['m_Component']['m_PathID'], read(f"level39-{ref['m_Component']['m_PathID']}.json"))
                      for ref in go['m_Components']]

        def one(predicate):
            matches = [(ident, value) for ident, value in components if predicate(value)]
            if len(matches) != 1:
                raise ValueError('Missing/ambiguous source component')
            return matches[0]

        _, transform = one(lambda d: 'm_LocalPosition' in d)
        _, renderer = one(lambda d: 'm_SortingOrder' in d)
        _, sprite = one(lambda d: '_spriteId' in d.get('m_Structure', {}))
        _, animator = one(lambda d: 'defaultClipId' in d.get('m_Structure', {}))
        collider_id, collider = one(lambda d: 'm_Size' in d and 'm_IsTrigger' in d)
        sprite, animator = sprite['m_Structure'], animator['m_Structure']
        if (not renderer['m_Enabled'] or not collider['m_Enabled'] or animator['isRealtime'] or
                animator['spriteOverride']['m_PathID'] or sprite['collection'] != dict(m_FileID=8, m_PathID=180) or
                animator['library'] != dict(m_FileID=8, m_PathID=197) or
                renderer['m_Materials'] != [dict(m_FileID=8, m_PathID=6)]):
            raise ValueError('Unsupported source renderer/animator')
        chain, seen = [transform], set()
        parent = transform['m_Father']
        while parent['m_PathID']:
            if parent['m_FileID'] or parent['m_PathID'] in seen:
                raise ValueError('External/cyclic source transform')
            seen.add(parent['m_PathID'])
            t = read(f"level39-{parent['m_PathID']}.json")
            ancestor = read(f"level39-{t['m_GameObject']['m_PathID']}.json")
            if not ancestor['m_IsActive']:
                raise ValueError('Inactive source ancestor')
            chain.append(t)
            parent = t['m_Father']
        origin = project((0, 0, 0), chain)
        sx, sy = sprite['_scale']['x'], sprite['_scale']['y']
        x, y = project((sx, 0, 0), chain), project((0, sy, 0), chain)
        rgba = [sprite['_color'][c] for c in 'rgba']
        values = [x[0]-origin[0], y[0]-origin[0], origin[0], x[1]-origin[1], y[1]-origin[1], origin[1],
                  origin[2], *rgba, renderer['m_SortingLayer'], renderer['m_SortingOrder'],
                  x[2]-origin[2], y[2]-origin[2], 0]
        if not all(math.isfinite(v) and abs(v) <= 100000 for v in values):
            raise ValueError('Invalid source transform/tint')
        if sprite['_spriteId'] not in remap:
            raise ValueError('Source idle pose absent from animation bank')
        if owner != 1180 and (collider['m_IsTrigger'] or animator['playAutomatically']):
            raise ValueError('Unsupported gate startup')
        parts.append(dict(owner=owner, values=values, collider=collider_id,
                          idle=remap[sprite['_spriteId']], collider_source=collider,
                          chain=chain, animator=animator))
    switch = parts[0]
    collider = switch['collider_source']
    if not collider['m_IsTrigger']:
        raise ValueError('Switch must use its authored trigger')
    offset, size = collider['m_Offset'], collider['m_Size']
    points = [project((offset['m_X']+sx*size['m_X']/2, offset['m_Y']+sy*size['m_Y']/2, 0), switch['chain'])
              for sx in (-1, 1) for sy in (-1, 1)]
    # The authored quaternion is approximately 180 degrees (1.6e-6 corner
    # deviation). Preserve its enclosing bounds; reject truly rotated triggers.
    if abs(switch['values'][1]) > .00001 or abs(switch['values'][3]) > .00001:
        raise ValueError('Rotated switch trigger needs polygon support')
    trigger = [min(p[0] for p in points), min(p[1] for p in points),
               max(p[0] for p in points), max(p[1] for p in points)]
    fsm = decode_fsm(read('level39-4997.json')['m_Structure']['fsm'])

    def action(state, kind):
        matches = [a['parameters'] for s in fsm['states'] if s['name'] == state
                   for a in s['actions'] if a['enabled'] and a['type'].split('.')[-1] == kind]
        if len(matches) != 1:
            raise ValueError('Changed source switch action')
        return matches[0]

    wait = action('Hit', 'Wait')
    freeze = action('Hit', 'SendMessage')['functionCall']
    frame = action('Activate', 'Tk2dPlayFrame')['frame']
    if (wait['realTime'] or wait['time']['useVariable'] or freeze['FunctionName'] != 'FreezeMoment' or
            freeze['IntParameter']['useVariable'] or frame['useVariable'] or freeze['IntParameter']['value'] != 1):
        raise ValueError('Unsupported source timing')
    auto = switch['animator']['playAutomatically']
    if auto and library['clips'][switch['animator']['defaultClipId']]['name'] != 'Switch Idle':
        raise ValueError('Changed source idle animation')
    result = dict(scene='Crossroads_03', id='Toll Gate Switch', trigger=trigger,
                  wait=wait['time']['value'], freeze=freeze['IntParameter']['value'], frame=frame['value'],
                  colliders=[p['collider'] for p in parts[1:]], idle=[p['idle'] for p in parts], play_idle=auto,
                  draws=[dict(owner=p['owner'], values=p['values']) for p in parts],
                  material_source=dict(collection='sharedassets9.assets', path_id=6),
                  approximation='Normal material pending external shader and original-player visual admission; trigger uses source quaternion AABB')
    return result


def room_bytes(data):
    raw = bytearray(struct.pack('<4s32s32sf2I4f2I3II', b'WSR1', data['scene'].encode('ascii'),
                                data['id'].encode('ascii'), data['wait'], data['freeze'], data['frame'],
                                *data['trigger'], *data['colliders'], *data['idle'], data['play_idle']))
    for draw in data['draws']:
        raw.extend(struct.pack('<I16f', draw['owner'], *draw['values']))
    return raw
