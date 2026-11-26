"""Compile the Waterways arena from the original room/additive exports."""
import json
import struct
from convert_terrain import paths, transform_point


def arena_floor(shapes, x, ceiling):
    surfaces = []
    for shape in shapes:
        points = shape['points']
        for a, b in zip(points, points[1:] + points[:1]):
            if a[0] == b[0] or not min(a[0], b[0]) <= x <= max(a[0], b[0]):
                continue
            y = a[1] + (b[1] - a[1]) * (x - a[0]) / (b[0] - a[0])
            if y <= ceiling:
                surfaces.append(y)
    if not surfaces:
        raise ValueError('Missing Dung Defender arena floor')
    return max(surfaces)


def compile_room(source, target, inventory, read, report):
    def bounds(row):
        points = [transform_point(p, [read(k) for k in row['chain']])
                  for contour in paths(read(row['collider']), row['type']) for p in contour]
        return [min(p[0] for p in points), min(p[1] for p in points),
                max(p[0] for p in points), max(p[1] for p in points)]

    visual = json.loads((source / 'visuals/room-visuals-map.json').read_text())['inventory']
    gates = []
    for name in ('Battle Gate (1)', 'Battle Gate'):
        rows = [r for r in inventory if read(r['go'])['m_Name'] == name]
        if len(rows) != 1:
            raise ValueError('Missing Dung Defender battle gate: ' + name)
        row = rows[0]
        owners = {row['go']}
        for r in visual:
            if row['go'] in r['owners']:
                owners.update(r['owners'][:r['owners'].index(row['go']) + 1])
        gates.append(dict(collider=int(row['collider'].split('-')[-1].split('.')[0]),
                          bounds=bounds(row), owners=sorted(int(o.split('-')[-1].split('.')[0]) for o in owners)))
    # level355 Transform27 is a root, not an offset under Battle Scene.
    boss_source = source.parent / 'Waterways_05_boss/terrain'
    transform = json.loads((boss_source / 'level355-27.json').read_text())
    p, s = transform['m_LocalPosition'], transform['m_LocalScale']
    position = [p['m_X'], p['m_Y']]
    # The saved boss starts buried below the walking surface. Cast down from
    # the lower battle gate, rather than treating that buried pose as ground.
    floor = arena_floor(report['shapes'], position[0], min(g['bounds'][1] for g in gates))
    # Battle Scene GO665's own trigger; its descendants include unrelated volumes.
    trigger_row = next(r for r in inventory if r['collider'] == 'level354-2025.json')
    trigger = bounds(trigger_row)
    arena = [gates[0]['bounds'][2], floor, gates[1]['bounds'][0], trigger[3]]
    room = dict(arena=arena, trigger=trigger, gates=gates, floor_colliders=[], floor_owners=[], broken_floor_owners=[])
    data = bytearray(struct.pack('<4s8fI', b'HKB2', *arena, *trigger, len(gates)))
    def ids(values):
        return struct.pack('<I', len(values)) + struct.pack('<' + 'I' * len(values), *values)
    for gate in gates:
        data += struct.pack('<I4f', gate['collider'], *gate['bounds']) + ids(gate['owners'])
    data += ids([]) * 3
    (target / 'boss-room.bin').write_bytes(data)
    # Additive scene namespace matches convert_room_visuals.additive_owner_id.
    from convert_room_visuals import additive_owner_id
    spawn = dict(owner=additive_owner_id(10), prefab='Dung Defender', position=position,
                 scale=[s['m_X'], s['m_Y']], active=True)
    return spawn, room
