"""Source-bound Greenpath vine footholds, using the falling terrain owner."""
import json
from scene_activation import fnv1a32, owner_id


def observe(mapping, read, visual_source, require):
    # Admit source inventories exercised by Stage A; other rooms retain their audit.
    expected = {'Fungus1_06': 7, 'Fungus1_21': 1}.get(mapping['scene'])
    if expected is None:
        return [], {}
    from convert_terrain import paths, transform_point
    rows = mapping['inventory']
    physics = read(mapping['physics'])
    require(physics['m_Gravity']['m_X'] == 0 and physics['m_Gravity']['m_Y'] < 0,
            'Unsupported vine platform gravity')
    visuals = json.loads((visual_source / 'room-visuals-map.json').read_text())
    motions, colliders = [], {}
    def contour(row):
        contours = paths(read(row['collider']), row['type'])
        require(len(contours) == 1 and len(contours[0]) == 4, 'Unsupported vine platform contour')
        return [transform_point(p, [read(k) for k in row['chain']]) for p in contours[0]]
    for root in rows:
        group = root['components'][0]
        if 'VinePlatform' not in group:
            continue
        # Inactive source variants are not footholds in a fresh room.
        if not all(read(owner)['m_IsActive'] for owner in root['owners']):
            continue
        source = read(group['VinePlatform'][0])['m_Structure']
        require(source['collider']['m_PathID'] == owner_id(root['collider']),
                'Changed vine platform solid binding')
        body = read(group['Rigidbody2D'][0])
        require(body['m_BodyType'] == 1 and body['m_Simulated'] and
                body['m_GravityScale'] == 2 and body['m_LinearDamping'] == 0 and
                body['m_Constraints'] == 4, 'Unsupported vine platform body')
        require(not source['acidLander'], 'Acid landing needs its own endpoint')
        landed = next(r for r in rows if owner_id(r['go']) == source['activatedSprite']['m_PathID'])
        vines = [r for r in rows if root['go'] in r['owners'] and
                 'VinePlatformCut' in r['components'][0]]
        require(len(vines) == 1, 'Missing vine platform cut trigger')
        vine = vines[0]
        cut = read(vine['components'][0]['VinePlatformCut'][0])['m_Structure']
        require(cut['body']['m_PathID'] == owner_id(group['Rigidbody2D'][0]) and
                read(vine['collider'])['m_IsTrigger'], 'Changed vine cut body binding')
        points = contour(root)
        initial = transform_point([0, 0], [read(k) for k in root['chain']])[1]
        lower = initial + max(p[1] for p in contour(landed)) - max(p[1] for p in points)
        require(lower < initial, 'Vine platform must fall down')
        item = read(group['PersistentBoolItem'][0])['m_Structure']
        require(not item['dontSave'] and not item['semiPersistent'], 'Unsupported vine persistence')
        saved = item['persistentBoolData']
        sprite = source['platformSprite']['m_PathID']
        hidden_roots = {owner_id(vine['go']), cut['sprites']['m_PathID']}
        moving, hidden = set(), set()
        for row in visuals['inventory']:
            owners = {owner_id(k) for k in row['owners']}
            if sprite in owners:
                moving.add(owner_id(row['go']))
            elif owners & hidden_roots:
                hidden.add(owner_id(row['go']))
        index = len(motions)
        colliders[root['collider']] = index
        motions.append(dict(kind=2, key=[fnv1a32(saved['sceneName'] or mapping['scene']),
                                        fnv1a32(saved['id'] or read(root['go'])['m_Name'])],
                            shape_ids=[owner_id(root['collider'])], lower=lower, upper=initial,
                            initial=initial, speed=physics['m_MaxTranslationSpeed'],
                            acceleration=-physics['m_Gravity']['m_Y'] * body['m_GravityScale'],
                            owners=sorted(moving), chains=sorted(hidden), stops=[initial, lower],
                            controls=[contour(vine)] * 3, upper_control=[0., 0.], lower_control=[0., 0.]))
    require(len(motions) == expected, 'Changed Stage A vine platform inventory')
    return motions, colliders
