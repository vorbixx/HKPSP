"""Fungus2_15's five persistent nail levers and their remote gates."""
import hashlib
import json


# Filled from the reviewed Switch Control / Mantis Gate exports, including
# their target references. A changed FSM must be reviewed before conversion.
MANTIS_FSMS = {
    'level179-8669.json': '4fbfc59c2487997f5a750457ef7f6b1f7e12b63e25676d10d307d815e529fc26',
    'level179-8688.json': 'fc86c34537dd1d0ac6237669519ca6a53290ef65f9aaae2e436b06786b5eab7f',
    'level179-8690.json': '8cac03b937ab99323ea2ad8377f401c9dabf28600fede43e31ed7c73695961f3',
    'level179-8746.json': '882fde64d20023deef3a0ecc96b6c94ae6f1824b657404615dab45e4f711eb5a',
    'level179-8763.json': 'd37b963735f35984eb2acda5f04bf7c2c0ac8c7c1fd70bc3e5a1173cfb4d2be1',
    'level179-8666.json': '5ae1f323a58154d6460d507b0f5ceb30cc153f25ed6368bd7ba8bc8aa47cea90',
    'level179-8712.json': '52225c1c239e7e0c6b36c5d88917ac0dd6e19e2f29ebdc88e4b4e288a83e2278',
    'level179-8750.json': '3070a0657d652b3a6990a945d04a89c41a6f0234d77bcba190ddf58267ab59cd',
    'level179-8749.json': '5e8fd14b4abe85cf5953d4797bf78c91571d9b43a2c2d9255d14d9b3b268d8c3',
    'level179-8823.json': 'aaecae0f5259ff1832a0ceb506039ec6256f0d08b838cb01c2a99ba89b119390',
}


def observe_mantis_switches(mapping, read, require):
    from scene_walls import identity, key
    from unity_transform import project
    require(mapping['scene'] == 'Fungus2_15', 'Unreviewed Mantis switch scene')
    groups = {go: group for row in mapping['inventory']
              for go, group in zip(row['owners'], row['components'])}
    gates, levers, observed = {}, {}, set()
    for go, group in groups.items():
        for name in group.get('PlayMakerFSM', []):
            fsm = read(name)['m_Structure']['fsm']
            if fsm['name'] not in ('Switch Control', 'Mantis Gate'):
                continue
            digest = hashlib.sha256(json.dumps(fsm, sort_keys=True).encode()).hexdigest()
            require(MANTIS_FSMS.get(name) == digest, 'Unreviewed Mantis FSM: ' + name)
            observed.add(name)
            (gates if fsm['name'] == 'Mantis Gate' else levers)[go] = fsm
    require(observed == set(MANTIS_FSMS) and len(gates) == len(levers) == 5,
            'Expected five reviewed Mantis lever/gate pairs')
    by_id = {identity(go): go for go in gates}
    result, targets = [], set()
    for go, fsm in levers.items():
        def variable(kind, name):
            return next(v['value'] for v in fsm['variables'][kind] if v['name'] == name)
        require(not variable('boolVariables', 'Activated') and
                not variable('boolVariables', 'SetPlayerData'), 'Unsupported Mantis initial state')
        target = variable('gameObjectVariables', 'Target')
        require(target['m_FileID'] == 0 and target['m_PathID'] in by_id and
                target['m_PathID'] not in targets, 'Unsupported Mantis target')
        targets.add(target['m_PathID'])
        gate = by_id[target['m_PathID']]
        rows = [r for r in mapping['inventory'] if r['go'] == gate and r['type'] == 'BoxCollider2D']
        triggers = [r for r in mapping['inventory'] if r['go'] == go and r['type'] == 'PolygonCollider2D']
        require(len(rows) == len(triggers) == 1, 'Unsupported Mantis collider layout')
        row, trigger = rows[0], triggers[0]
        require(read(row['collider'])['m_Enabled'] and not read(row['collider'])['m_IsTrigger'],
                'Unsupported Mantis gate collision')
        collider = read(trigger['collider'])
        paths = collider['m_Points']['m_Paths']
        require(collider['m_Enabled'] and collider['m_IsTrigger'] and len(paths) == 1 and
                3 <= len(paths[0]) <= 8, 'Unsupported Mantis nail trigger')
        x, y = (collider['m_Offset'][k] for k in ('m_X', 'm_Y'))
        chain = [read(n) for n in trigger['chain']]
        polygon = [project((x + point['m_X'], y + point['m_Y'], 0), chain)[:2]
                   for point in paths[0]]
        persistent_names = groups[go].get('PersistentBoolItem', [])
        require(len(persistent_names) == 1, 'Missing Mantis switch persistence')
        persistent = read(persistent_names[0])['m_Structure']
        require(not persistent['semiPersistent'] and not persistent['dontSave'],
                'Unsupported Mantis switch persistence')
        saved = persistent['persistentBoolData']
        # Source waits: lever .75, gate pause .5, dust .7. Animation library
        # sharedassets168.assets:49: Door Open 1 = 4/20 s, Open 2 = 2/12 s.
        collision_delay = .75 + .5 + .7 + 4 / 20
        result.append(dict(kind=1, root=identity(gate),
                           scene=key(saved['sceneName'] or mapping['scene']),
                           object=key(saved['id'] or read(go)['m_Name']), player_data=0,
                           hits=1, facing=0, collider=identity(row['collider']),
                           origin=project((0, 0, 0), [read(n) for n in row['chain']]),
                           children=[], locks=[], nail_trigger=polygon,
                           collision_delay=collision_delay, hide_delay=collision_delay + 2 / 12))
    return result
