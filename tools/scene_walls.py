"""Source-checked persistent breakable walls; unknown FSMs remain guarded."""
import hashlib
import json
import struct

# These exact exported FSMs were inspected, including attack-type dispatch,
# recoil, persistence, child UNCOVER/BREAK and the additional PlayerData hook.
WALL_FSMS = {
    'Abyss_03_c': '13b271978ff3416e1340499376bc42a2fea9e88430b30a5eb75d06dc17e81baf',
    'Abyss_05': '54b197627706b03f25d213302e4e255c2cb7d1ae8f6271ae5a39d693ed4c2884',
    'Fungus2_15': 'fcbb0777590469238c0ca6fe61933ac00f11d1affd462f19e67ebed6dfa2b531',
    'Fungus2_20': '4f2d61d613acc2b5b93b2d86632bb2b1f0bafdfd2b5ba06b8d1677ca9f51f7e6',
    'Ruins_Bathhouse': '304933fbed4778d888e432b92d1af4692458e52846b5992c4de3773af13f9143',
    'Ruins1_31': 'e67f69cad01d4c4bb884f3b8b3a6364e14148dab345454a6b91a0caf251ac7fd',
    'Ruins2_03': 'a7799c53b443b4efcb5ff0584ce79da5d32aca076e9dea2815d929645034bf0f',
}


def key(name):
    value = 2166136261
    for byte in name.encode('utf-8'):
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def identity(name):
    return int(name.removesuffix('.json').rsplit('-', 1)[1])


def observe_crossroads_floor(mapping, read, visual_source, require):
    """Crossroads_04 break_floor: separate solid, nail polygon and Hero Range."""
    from convert_terrain import paths, transform_point
    from unity_transform import project
    for name, digest in (
            ('level40-5039.json', '43ed4518de0ac51f84e03db1e97816c5648271243cbc14fb12ea51a72038dfb8'),
            ('level40-5040.json', 'cd938d2cc336f11354ee489cd4c7872fd6f45021aac12ac509dabf02d9ccd4eb')):
        fsm = read(name)['m_Structure']['fsm']
        require(hashlib.sha256(json.dumps(fsm, sort_keys=True).encode()).hexdigest() == digest,
                'Unreviewed Crossroads break_floor FSM')
    rows = mapping['inventory']
    def polygon(number, kind):
        row = next(r for r in rows if identity(r['collider']) == number)
        require(row['type'] == kind, 'Changed Crossroads floor collider')
        contours = paths(read(row['collider']), kind)
        require(len(contours) == 1, 'Changed Crossroads floor contours')
        return [transform_point(p, [read(n) for n in row['chain']]) for p in contours[0]]
    hit = polygon(3253, 'PolygonCollider2D')
    require(len(hit) == 5, 'Changed Crossroads floor nail trigger')
    # This authored concave polygon is star-shaped about its final vertex.
    triangles = [[hit[-1], hit[i], hit[i+1]] for i in range(len(hit)-2)]
    turns = [(b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]) for a,b,c in triangles]
    require(all(t < 0 for t in turns), 'Changed Crossroads floor triangulation')
    hero = polygon(3301, 'BoxCollider2D')
    def bounds(points):
        return [min(p[0] for p in points), min(p[1] for p in points),
                max(p[0] for p in points), max(p[1] for p in points)]
    root = next(r for r in rows if identity(r['collider']) == 3253)
    require(root['go'] == 'level40-473.json' and read(root['go'])['m_Name'] == 'Break Floor 1',
            'Changed Crossroads floor identity')
    persistent = read('level40-5693.json')['m_Structure']
    require(not persistent['semiPersistent'] and not persistent['dontSave'], 'Changed floor persistence')
    data = persistent['persistentBoolData']
    visuals = json.loads((visual_source / 'room-visuals-map.json').read_text())
    children = sorted({identity(r['go']) for r in visuals['inventory']
                       if root['go'] in r['owners'] and r['go'] != root['go']})
    wall = dict(root=473, scene=key(data['sceneName'] or mapping['scene']),
                object=key(data['id'] or 'Break Floor 1'), player_data=0, hits=3, facing=3,
                collider=3300, origin=project((0,0,0), [read(n) for n in root['chain']]),
                children=children, locks=[], one_way=True, hit_wait=.25,
                hit_bounds=bounds(hit), hero_range=bounds(hero), hit_triangles=triangles)
    return [wall], {3300: wall}


def observe_one_way_wall(mapping, read, visual_source, require):
    """Reviewed break_floor instances; the Hero Range gates ordinary nail hits."""
    basin = mapping['scene'] == 'Abyss_19'
    root_id, hit_id, solid_id, range_id, detector, persistence = (
        (1525, 6144, 6146, 6145, 1527, 10711) if basin else
        (531, 2009, 2010, 2012, 529, 3110))
    prefix = 'level343' if basin else 'level354'
    fsm_name = 'level343-9805.json' if basin else 'level354-2995.json'
    fsm_digest = ('9115b53f86864cb9de79c405c7134196f7bab531911e697a8f6594b9c92fd98b' if basin else
                  '9658e127f6583d3a18170ca57c1d19179005187ddcf9aa903822c9725988e1a0')
    range_fsm = 'level343-9806.json' if basin else 'level354-2994.json'
    from unity_transform import project
    rows = mapping['inventory']
    root_row = next(r for r in rows if identity(r['collider']) == hit_id)
    root = root_row['go']
    require(identity(root) == root_id and read(root)['m_Name'] == 'One Way Wall',
            'Changed one-way wall identity')
    for name, digest in (
            (fsm_name, fsm_digest),
            (range_fsm, 'cd938d2cc336f11354ee489cd4c7872fd6f45021aac12ac509dabf02d9ccd4eb')):
        fsm = read(name)['m_Structure']['fsm']
        require(hashlib.sha256(json.dumps(fsm, sort_keys=True).encode()).hexdigest() == digest,
                'Unreviewed one-way wall FSM')
    persistent = read(f'{prefix}-{persistence}.json')['m_Structure']
    require(not persistent['semiPersistent'] and not persistent['dontSave'],
            'Unsupported one-way wall persistence')
    def bounds(collider):
        row = next(r for r in rows if identity(r['collider']) == collider)
        c = read(row['collider'])
        require(row['type'] == 'BoxCollider2D', 'Changed one-way wall shape')
        o, size = c['m_Offset'], c['m_Size']
        points = [project((o['m_X'] + x * size['m_X'] / 2,
                           o['m_Y'] + y * size['m_Y'] / 2, 0),
                          [read(n) for n in row['chain']]) for x,y in ((-1,-1),(1,-1),(1,1),(-1,1))]
        return [min(p[0] for p in points), min(p[1] for p in points),
                max(p[0] for p in points), max(p[1] for p in points)]
    descendants = [r for r in rows if root in r['owners'] and r['go'] != root]
    require(all(read(r['collider'])['m_IsTrigger'] or identity(r['collider']) == solid_id or
                (not basin and identity(r['collider']) == 2011 and not read(r['go'])['m_IsActive'])
                for r in descendants), 'Unreviewed extra one-way wall solid')
    require(read(f'{prefix}-{detector}.json')['m_Layer'] == 13, 'Changed Hero Detector layer')
    visuals = json.loads((visual_source / 'room-visuals-map.json').read_text())
    children = sorted({identity(r['go']) for r in visuals['inventory']
                       if root in r['owners'] and r['go'] != root})
    locks = [identity(r['go']) for r in descendants if 'CameraLockArea' in r['components'][0]]
    data = persistent['persistentBoolData']
    wall = dict(root=root_id, object=key(data['id'] or read(root)['m_Name']),
                scene=key(data['sceneName'] or mapping['scene']), player_data=0 if basin else key('dungDefenderWallBroken'),
                hits=3, facing=0, collider=solid_id,
                origin=project((0,0,0), [read(n) for n in root_row['chain']]),
                children=children, locks=locks, one_way=True,
                hit_bounds=bounds(hit_id), hero_range=bounds(range_id))
    return [wall], {solid_id: wall}


def observe_break_floors(mapping, read, visual_source, require):
    """All ordinary three-hit break_floor variants use the Tutorial floor contract."""
    from convert_breakable import hit_triangles
    from convert_terrain import paths, transform_point
    from unity_transform import project
    visual_map = visual_source / 'room-visuals-map.json'
    result, controlled = [], {}
    for row in mapping['inventory']:
        go = read(row['go'])
        if go['m_Name'] not in ('Break Floor 1', 'One Way Wall'):
            continue
        group = row['components'][0]
        fsms = group.get('PlayMakerFSM', [])
        persistent = group.get('PersistentBoolItem', [])
        if len(fsms) != 1 or len(persistent) != 1:
            continue
        fsm = read(fsms[0])['m_Structure']['fsm']
        if fsm['name'] != 'break_floor':
            continue
        state_names = {state['name'] for state in fsm['states']}
        require({'Idle', 'Hit', 'Break', 'Hit 1', 'Hit 2', 'Activated'} <= state_names,
                'Unsupported break_floor states')
        variables = fsm['variables']
        ints = {value['name']: value['value'] for value in variables['intVariables']}
        bools = {value['name']: value['value'] for value in variables['boolVariables']}
        require(ints.get('Hits') == 0 and not bools.get('Activated', False),
                'Unsupported break_floor initial state')
        pdata = read(persistent[0])['m_Structure']
        require(not pdata['semiPersistent'] and not pdata['dontSave'],
                'Unsupported break_floor persistence')

        def polygon(collider_row):
            shape = read(collider_row['collider'])
            contours = paths(shape, collider_row['type'])
            require(len(contours) == 1, 'Changed break_floor collider contours')
            return [transform_point(point, [read(name) for name in collider_row['chain']])
                    for point in contours[0]]

        hit = polygon(row)
        triangles = hit_triangles([hit])
        require(0 < len(triangles) <= 8, 'Break floor trigger exceeds runtime budget')
        descendants = [candidate for candidate in mapping['inventory']
                       if row['go'] in candidate['owners'] and candidate['go'] != row['go']]
        solids = [candidate for candidate in descendants
                  if read(candidate['go'])['m_Name'] == 'Solid' and
                     read(candidate['go'])['m_IsActive'] and
                     not read(candidate['collider'])['m_IsTrigger'] and
                     candidate['type'] == 'BoxCollider2D']
        require(solids, 'Break floor has no active solid')
        solid_ids = [identity(candidate['collider']) for candidate in solids]
        hero_rows = [candidate for candidate in descendants
                     if read(candidate['go'])['m_Name'] == 'Hero Range' and
                        read(candidate['go'])['m_IsActive']]
        require(len(hero_rows) <= 1, 'Break floor has multiple Hero Range triggers')
        if hero_rows:
            hero_points = polygon(hero_rows[0])
            hero_range = [min(p[0] for p in hero_points), min(p[1] for p in hero_points),
                          max(p[0] for p in hero_points), max(p[1] for p in hero_points)]
        else:
            hero_range = [min(p[0] for p in hit), min(p[1] for p in hit),
                          max(p[0] for p in hit), max(p[1] for p in hit)]
        require(visual_map.is_file(), 'Break floor visual export missing')
        visuals = json.loads(visual_map.read_text())
        children = sorted({identity(candidate['go']) for candidate in visuals['inventory']
                           if row['go'] in candidate['owners'] and candidate['go'] != row['go']})
        locks = [identity(candidate['go']) for candidate in descendants
                 if 'CameraLockArea' in candidate['components'][0]]
        data = pdata['persistentBoolData']
        wall = dict(root=identity(row['go']), scene=key(data['sceneName'] or mapping['scene']),
                    object=key(data['id'] or go['m_Name']), player_data=0, hits=3, facing=3,
                    collider=solid_ids[0], origin=project((0, 0, 0), [read(name) for name in row['chain']]),
                    children=children, locks=locks, one_way=True, hit_wait=.25,
                    hit_bounds=[min(p[0] for p in hit), min(p[1] for p in hit),
                                max(p[0] for p in hit), max(p[1] for p in hit)],
                    hero_range=hero_range, hit_triangles=triangles,
                    linked_colliders=solid_ids[1:], reject_down=not hero_rows)
        result.append(wall)
        controlled.update({collider: wall for collider in solid_ids})
    return result, controlled


def observe_walls(mapping, read, visual_source, require):
    result, colliders = _observe_walls(mapping, read, visual_source, require)
    # TerrainBank assigns each shape to one wall only; a second claim makes the room fail to load.
    claimed = [c for wall in result for c in (wall['collider'], *wall.get('linked_colliders', ()))]
    require(len(claimed) == len(set(claimed)), 'A terrain collider belongs to two walls')
    return result, colliders


def _observe_walls(mapping, read, visual_source, require):
    result, colliders = observe_break_floors(mapping, read, visual_source, require)
    if mapping['scene'] in ('Deepnest_16', 'Deepnest_30', 'Deepnest_41'):
        from scene_collapse import observe
        extra, extra_colliders = observe(mapping, read, visual_source, require)
        # A collapse floor with a Hero Range (Deepnest_41) is also a break_floor; the collapse
        # contract owns it, so the plain break-floor record would assign its Solid twice.
        owned = {wall['root'] for wall in extra}
        result = [wall for wall in result if wall['root'] not in owned]
        colliders = {collider: wall for collider, wall in colliders.items() if wall['root'] not in owned}
        result.extend(extra);colliders.update(extra_colliders)
        return result, colliders
    if mapping['scene'] not in WALL_FSMS:
        return result, colliders
    from inspect_fsm import decode_fsm
    from unity_transform import project
    visuals = json.loads((visual_source / 'room-visuals-map.json').read_text())
    for row in mapping['inventory']:
        group = row['components'][0]
        go = read(row['go'])
        if not go['m_Name'].startswith('Breakable Wall'):
            continue
        require(row['type'] == 'BoxCollider2D' and len(group.get('PlayMakerFSM', [])) == 1,
                'Unsupported wall collider/FSM')
        fsm = read(group['PlayMakerFSM'][0])['m_Structure']['fsm']
        digest = hashlib.sha256(json.dumps(fsm, sort_keys=True).encode()).hexdigest()
        require(digest == WALL_FSMS[mapping['scene']], 'Unreviewed wall FSM')
        def variable(kind, name):
            return next(v['value'] for v in fsm['variables'][kind] if v['name'] == name)
        entry_left_of = None
        if variable('boolVariables', 'Ruin Lift'):
            require(mapping['scene'] == 'Ruins1_31', 'Unreviewed Ruin Lift wall policy')
            state = next(s for s in decode_fsm(fsm)['states'] if s['name'] == 'Ruin Lift?')
            compare = next(a['parameters'] for a in state['actions']
                           if a['enabled'] and a['type'].endswith('.FloatCompare'))
            require(compare['float1'] == {'variable': 'Hero X'} and
                    compare['lessThan'] == 'RUIN LIFT' and not compare['everyFrame'],
                    'Unsupported wall entry comparison')
            entry_left_of = compare['float2']['value']
        require(len(group.get('Rigidbody2D', [])) == 1, 'Missing wall body')
        body = read(group['Rigidbody2D'][0])
        require(body['m_BodyType'] == 1 and body['m_GravityScale'] == 0 and body['m_Simulated']
                and body['m_Constraints'] == 4, 'Unsupported wall body')
        require(len(group.get('PersistentBoolItem', [])) == 1, 'Missing wall persistence')
        persistent = read(group['PersistentBoolItem'][0])['m_Structure']
        require(not persistent['semiPersistent'] and not persistent['dontSave'], 'Unsupported wall persistence')
        root = identity(row['go'])
        children = []
        for visual in visuals['inventory']:
            if row['go'] not in visual['owners'] or visual['go'] == row['go']:
                continue
            fsms = [decode_fsm(json.loads((visual_source / f).read_text())['m_Structure']['fsm'])
                    for f in visual['components'].get('PlayMakerFSM', [])]
            masks = [f for f in fsms if f['name'] == 'unmasker']
            require(len(masks) == 1, 'Unsupported wall child renderer')
            fades = [a['parameters'] for s in masks[0]['states'] for a in s['actions']
                     if a['enabled'] and a['type'].endswith('.iTweenFadeTo') and
                     a['parameters']['time']['value'] == .5]
            require(fades and all(p['alpha']['value'] == 0 and p['includeChildren']['value'] and
                    p['easeType'] == 21 and p['delay']['value'] == 0 for p in fades),
                    'Unsupported wall mask fade')
            # The reviewed child masks have no colliders: their Player-stay
            # action cannot fire. UNCOVER/BREAK comes from the parent wall.
            require(not any('Collider2D' in k for k in visual['components']),
                    'Wall mask needs player-trigger simulation')
            children.append(identity(visual['go']))
        locks = [identity(r['go']) for r in mapping['inventory']
                 if row['go'] in r['owners'] and 'CameraLockArea' in r['components'][0]]
        name = persistent['persistentBoolData']['id'] or go['m_Name']
        scene = persistent['persistentBoolData']['sceneName'] or mapping['scene']
        pd = variable('stringVariables', 'PlayerData Bool')
        origin = project((0, 0, 0), [read(n) for n in row['chain']])
        result.append(dict(root=root, object=key(name), scene=key(scene), player_data=key(pd) if pd else 0,
                           hits=variable('intVariables', 'Hits'), facing=variable('intVariables', 'Facing'),
                           collider=identity(row['collider']), origin=origin,
                           children=sorted(set(children)), locks=locks, entry_left_of=entry_left_of))
    require(len([wall for wall in result if not wall.get('one_way')]) == 1,
            'Expected one reviewed wall')
    if mapping['scene'] == 'Fungus2_15':
        from mantis_switches import observe_mantis_switches
        result.extend(observe_mantis_switches(mapping, read, require))
    colliders.update({wall['collider']: wall for wall in result})
    return result, colliders


def write_walls(walls, path):
    # v5 is the triangle layout; v6 also carries timed Mantis interactions.
    version = (6 if any(wall.get('kind', 0) or 'linked_colliders' in wall or 'contact_delay' in wall for wall in walls) else
               5 if any(wall.get('hit_triangles') for wall in walls) else
               4 if any(wall.get('one_way') for wall in walls) else
               3 if any(wall.get('entry_left_of') is not None for wall in walls) else 2)
    data = bytearray(struct.pack('<4sII', b'HKW1', version, len(walls)))
    for wall in walls:
        data += struct.pack('<7I', *(wall[k] for k in ('root', 'scene', 'object', 'player_data', 'hits', 'facing', 'collider')))
        data += struct.pack('<3f', *wall['origin'])
        if version >= 3:
            threshold = wall.get('entry_left_of')
            data += struct.pack('<If', int(threshold is not None), threshold if threshold is not None else 0)
        if version >= 4:
            data += struct.pack('<I8f', int(wall.get('one_way', False)),
                                *wall.get('hit_bounds', [0]*4), *wall.get('hero_range', [0]*4))
        if version >= 6:
            trigger = wall.get('nail_trigger', [])
            kind = wall.get('kind', 0) | (2 if wall.get('reject_down') else 0)
            data += struct.pack('<II', kind, len(trigger))
            data += struct.pack('<' + 'f' * (2 * len(trigger) + 2),
                                *(value for point in trigger for value in point),
                                wall.get('collision_delay', 0), wall.get('hide_delay', 0))
        if version >= 5:
            triangles = wall.get('hit_triangles', [])
            data += struct.pack('<fI', wall.get('hit_wait', .15 if wall.get('one_way') else .2), len(triangles))
            for triangle in triangles:
                data += struct.pack('<6f', *(v for p in triangle for v in p))
        for name in ('children', 'locks'):
            data += struct.pack('<I', len(wall[name]))
            for owner in wall[name]:
                data += struct.pack('<I', owner)
        if version >= 6:
            linked = wall.get('linked_colliders', [])
            kind = 2 if 'hero_range' in wall and not wall.get('one_way') else int('contact_delay' in wall)
            data += struct.pack('<IfI', kind, wall.get('contact_delay', 0), len(linked))
            for collider in linked:
                data += struct.pack('<I', collider)
            if version >= 5:
                data += struct.pack('<4f', *(wall['hero_range'] if kind == 2 else (0, 0, 0, 0)))
    path.write_bytes(data)
    return data
