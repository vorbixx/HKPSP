"""Source-bound persistent contact floors on the selected Deepnest route."""
import hashlib
import json
from scene_walls import identity, key

CONTACT_FLOORS = {
    'Deepnest_16': (
        ('Collapser Small', 'Collapser Small (1)'),
        'b0088619d9afca76f4b4156824c0ad854b80d7e38a65f29b9b192325ec861ea5'),
    'Deepnest_30': (
        ('Collapser Small top', 'Collapser Small', 'Collapser Small (1)',
         'Collapser Small (2)', 'Collapser Small (3)'),
        '01ca09aeeb96730c57d6a92b2f0858fae8db73cb0b3ca45cf231598c8d7b0c4f'),
    'Deepnest_41': (
        ('Collapser Small', 'Collapser Small (1)', 'Collapser Small (2)',
         'Collapser Small (4)'),
        '9da4bb4208ae4e9615b6f52c8f1b6d8324fc36a81dfd09725d1d9624ee4cbb84'),
}


def observe(mapping, read, visual_source, require):
    specification = CONTACT_FLOORS.get(mapping['scene'])
    if specification is None:
        return [], {}
    names, expected_digest = specification
    from unity_transform import project
    visuals = json.loads((visual_source / 'room-visuals-map.json').read_text())
    result = []
    for row in mapping['inventory']:
        go = read(row['go'])
        if go['m_Name'] not in names:
            continue
        group = row['components'][0]
        require(row['type'] == 'BoxCollider2D' and len(group.get('PlayMakerFSM', [])) == 1,
                'Unsupported contact floor collider/FSM')
        fsm = read(group['PlayMakerFSM'][0])['m_Structure']['fsm']
        digest = hashlib.sha256(json.dumps(fsm, sort_keys=True).encode()).hexdigest()
        require(digest == expected_digest,
                'Unreviewed contact floor FSM')
        require(len(group.get('PersistentBoolItem', [])) == 1, 'Missing floor persistence')
        persistent = read(group['PersistentBoolItem'][0])['m_Structure']
        require(not persistent['semiPersistent'] and not persistent['dontSave'],
                'Unsupported floor persistence')
        descendants = [r for r in mapping['inventory']
                       if row['go'] in r['owners'] and r['go'] != row['go']]
        linked = [r for r in descendants if read(r['go'])['m_Name'] == 'Bottom Block']
        for child in descendants:
            if child in linked:
                continue
            require(read(child['collider'])['m_IsTrigger'] and
                    read(child['go'])['m_Name'] in ('CameraLockArea', 'Inverse Remasker'),
                    'Unsupported contact floor descendant')
        locks = [identity(r['go']) for r in descendants
                 if read(r['go'])['m_Name'] == 'CameraLockArea']
        require(len(linked) == 1 and read(linked[0]['go'])['m_Name'] == 'Bottom Block' and
                linked[0]['type'] == 'BoxCollider2D', 'Unsupported contact floor child collision')
        children = sorted({identity(v['go']) for v in visuals['inventory']
                           if row['go'] in v['owners'] and v['go'] != row['go']})
        name = persistent['persistentBoolData']['id'] or go['m_Name']
        scene = persistent['persistentBoolData']['sceneName'] or mapping['scene']
        result.append(dict(root=identity(row['go']), object=key(name), scene=key(scene),
                           player_data=0, hits=1, facing=0, collider=identity(row['collider']),
                           origin=project((0, 0, 0), [read(n) for n in row['chain']]),
                           children=children, locks=locks, contact_delay=.4000000059604645,
                           linked_colliders=[identity(linked[0]['collider'])]))
    require(len(result) == len(names), 'Unexpected source contact floor count')
    if mapping['scene'] == 'Deepnest_41':
        result.extend(one_way_walls(mapping, read, visuals, require))
    return result, {floor['collider']: floor for floor in result}


def one_way_walls(mapping, read, visuals, require):
    """level299 break_floor: Hero Range gates three nail-trigger entries."""
    from unity_transform import project
    result = []
    for row in mapping['inventory']:
        if identity(row['go']) not in (368, 1181, 2369):
            continue
        group = row['components'][0]
        require(len(group.get('PlayMakerFSM', [])) == 1,
                'Missing one-way wall FSM')
        fsm = read(group['PlayMakerFSM'][0])['m_Structure']['fsm']
        require(hashlib.sha256(json.dumps(fsm, sort_keys=True).encode()).hexdigest() ==
                '4db87f52861c21de1f9ba02d1430c48b724c384e7028dc92080eaf802f9fb597',
                'Unreviewed one-way wall FSM')
        require(row['type'] == 'BoxCollider2D' and read(row['collider'])['m_IsTrigger'],
                'Unsupported one-way nail sensor')
        descendants = [r for r in mapping['inventory']
                       if row['go'] in r['owners'] and r['go'] != row['go']]
        solids = [r for r in descendants if read(r['go'])['m_Name'] == 'Solid']
        ranges = [r for r in descendants if read(r['go'])['m_Name'] == 'Hero Range']
        require(len(solids) == 2 and len(ranges) == 1 and
                all(r['type'] == 'BoxCollider2D' and not read(r['collider'])['m_IsTrigger']
                    for r in solids), 'Unsupported one-way wall children')
        active_solids = [r for r in solids if all(read(o)['m_IsActive'] for o in r['owners'])]
        require(len(active_solids) == 1, 'Unexpected active one-way solid')
        sensor = ranges[0]
        range_fsm = read(sensor['components'][0]['PlayMakerFSM'][0])['m_Structure']['fsm']
        require(hashlib.sha256(json.dumps(range_fsm, sort_keys=True).encode()).hexdigest() ==
                'cd938d2cc336f11354ee489cd4c7872fd6f45021aac12ac509dabf02d9ccd4eb',
                'Unreviewed one-way Hero Range FSM')
        collider = read(sensor['collider'])
        require(sensor['type'] == 'BoxCollider2D' and collider['m_IsTrigger'],
                'Unsupported one-way Hero Range')
        size, offset = collider['m_Size'], collider['m_Offset']
        points = [project((offset['m_X'] + x*size['m_X']/2,
                           offset['m_Y'] + y*size['m_Y']/2, 0),
                          [read(t) for t in sensor['chain']])
                  for x in (-1, 1) for y in (-1, 1)]
        bounds = [min(p[0] for p in points), min(p[1] for p in points),
                  max(p[0] for p in points), max(p[1] for p in points)]
        require(len(group.get('PersistentBoolItem', [])) == 1,
                'Missing one-way persistence')
        persistent = read(group['PersistentBoolItem'][0])['m_Structure']
        require(not persistent['semiPersistent'] and not persistent['dontSave'],
                'Unsupported one-way persistence')
        name = persistent['persistentBoolData']['id'] or read(row['go'])['m_Name']
        scene = persistent['persistentBoolData']['sceneName'] or mapping['scene']
        children = sorted({identity(v['go']) for v in visuals['inventory']
                           if row['go'] in v['owners'] and v['go'] != row['go']})
        result.append(dict(root=identity(row['go']), object=key(name), scene=key(scene),
                           player_data=0, hits=3, facing=3, collider=identity(row['collider']),
                           origin=project((0, 0, 0), [read(t) for t in row['chain']]),
                           children=children, locks=[], hero_range=bounds,
                           linked_colliders=[identity(r['collider']) for r in active_solids]))
    require(len(result) == 3, 'Unexpected one-way wall count')
    return result
