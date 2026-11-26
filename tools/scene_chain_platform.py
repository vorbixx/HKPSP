"""Source-bound Ruins1_28 cut-chain platform motion."""
import json
from scene_activation import fnv1a32, owner_id


def observe(mapping, read, visual_source, require):
    if mapping['scene'] != 'Ruins1_28':
        return [], {}
    from convert_terrain import paths, transform_point
    from inspect_fsm import decode_fsm
    rows = mapping['inventory']
    roots = [r for r in rows if read(r['go'])['m_Name'] == 'Chain Platform']
    require(len(roots) == 1, 'Missing chain platform')
    root = roots[0]
    group = root['components'][0]
    body = read(group['Rigidbody2D'][0])
    require(body['m_BodyType'] == 1 and body['m_GravityScale'] == 0 and body['m_Simulated'],
            'Unsupported chain platform body')
    fsm = read(group['PlayMakerFSM'][0])['m_Structure']['fsm']
    decoded = decode_fsm(fsm)
    fall = next(s for s in decoded['states'] if s['name'] == 'Fall')
    accel = next(a['parameters'] for a in fall['actions'] if a['enabled'] and a['type'].endswith('.AccelerateVelocity'))
    require(accel['yAccel']['value'] == -2 and accel['yMaxSpeed']['value'] == 60,
            'Changed chain platform fall motion')
    refs = {v['name']: v['value']['m_PathID'] for v in fsm['variables']['gameObjectVariables']}
    landed = next(r for r in rows if owner_id(r['go']) == refs['Activated Plat'])
    vine = next(r for r in rows if owner_id(r['go']) == refs['Vine'])
    def contour(row):
        contours = paths(read(row['collider']), row['type'])
        require(len(contours) == 1 and len(contours[0]) == 4, 'Unsupported chain platform contour')
        return [transform_point(p, [read(k) for k in row['chain']]) for p in contours[0]]
    # The moving body owns the authored platform and the inactive replacement
    # solid used after the persistent cut.  Both colliders inherit the same
    # Rigidbody2D through the transform chain and must follow the body.  The
    # activation bank controls which one is live at startup/after landing.
    moving_solids = []
    for row in rows:
        if root['go'] not in row['owners']:
            continue
        collider = read(row['collider'])
        owner = read(row['go'])
        if owner['m_Layer'] == 8 and not collider['m_IsTrigger'] and collider['m_Enabled']:
            moving_solids.append(row)
    require(moving_solids, 'Missing chain platform solids')
    points = contour(root)
    initial = transform_point([0, 0], [read(k) for k in root['chain']])[1]
    # The saved Activated branch uses the separate authored landed platform.
    # approximation: preserve the original moving contour at that top surface;
    # the saved replacement is slightly wider and taller in the source.
    lower = initial + max(p[1] for p in contour(landed)) - max(p[1] for p in points)
    require(lower < initial, 'Chain platform must fall down')
    visuals = json.loads((visual_source / 'room-visuals-map.json').read_text())
    hidden_roots = {refs['Vine'], refs['Hero Land']}
    moving, hidden = set(), set()
    for row in visuals['inventory']:
        if root['go'] not in row['owners']:
            continue
        owners = {owner_id(k) for k in row['owners']}
        (hidden if owners & hidden_roots else moving).add(owner_id(row['go']))
    item = read(group['PersistentBoolItem'][0])['m_Structure']
    require(not item['dontSave'] and not item['semiPersistent'], 'Unsupported platform persistence')
    saved = item['persistentBoolData']
    motion = dict(kind=1, key=[fnv1a32(saved['sceneName'] or mapping['scene']),
                             fnv1a32(saved['id'] or read(root['go'])['m_Name'])],
                  shape_ids=[owner_id(row['collider']) for row in moving_solids], lower=lower, upper=initial,
                  initial=initial, speed=60., acceleration=100.,
                  owners=sorted(moving), chains=sorted(hidden), stops=[initial, lower],
                  controls=[contour(vine), contour(vine), contour(vine)],
                  upper_control=[0., 0.], lower_control=[0., 0.])
    return [motion], {row['collider']: 0 for row in moving_solids}
