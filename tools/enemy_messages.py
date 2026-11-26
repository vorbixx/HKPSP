"""Discover authored enemy message zones by component, never by object name."""
import hashlib
import math
from convert_terrain import paths, transform_point, require


def observe_messages(inventory, read, matrix):
    result = []
    for row in inventory:
        local = row['components'][0]
        scripts = local.get('SendEnemyMessageTrigger', [])
        if not scripts:
            continue
        require(len(scripts) == 1, 'Multiple enemy message senders')
        script, collider, owner = read(scripts[0]), read(row['collider']), read(row['go'])
        require(not local.get('PlayMakerFSM'), 'Enemy message FSM override needs explicit support')
        require(script['m_Enabled'] and collider['m_Enabled'] and
                all(read(k)['m_IsActive'] for k in row['owners']), 'Dynamic enemy message lifecycle needs support')
        require(row['type'] == 'BoxCollider2D' and collider['m_IsTrigger'] and
                not collider['m_UsedByEffector'] and not collider['m_CompositeOperation'] and
                not collider.get('m_EdgeRadius', 0), 'Unsupported enemy message shape')
        require(all(collider[k]['m_Bits'] == 0xffffffff for k in
                    ('m_ForceSendLayers', 'm_ForceReceiveLayers', 'm_ContactCaptureLayers', 'm_CallbackLayers')) and
                all(collider[k]['m_Bits'] == 0 for k in ('m_IncludeLayers', 'm_ExcludeLayers')),
                'Enemy message collider filtering override')
        require(not any('Rigidbody2D' in group for group in row['components']), 'Moving enemy message zone')
        event = script['m_Structure']['eventName']
        require(event in ('GO LEFT', 'GO RIGHT', 'GO UP'), 'Unknown enemy message: ' + event)
        points = [transform_point(p, [read(k) for k in row['chain']]) for p in paths(collider, row['type'])[0]]
        xs, ys = sorted(set(p[0] for p in points)), sorted(set(p[1] for p in points))
        require(len(xs) == len(ys) == 2 and all(math.isfinite(v) for v in xs + ys),
                'Enemy message zone must be an axis aligned box')
        result.append(dict(source_go=row['go'], collider=row['collider'], event=event,
                           rect=[xs[0], ys[0], xs[1], ys[1]], layers=matrix[owner['m_Layer']]))
    return result


def load_messages(source, read_bytes=None):
    import json
    if read_bytes is None:
        read_bytes = lambda path: path.read_bytes()
    read = lambda name: json.loads(read_bytes(source / name))
    manifest = read('terrain-map.json')
    for name, expected in manifest['source_sha256'].items():
        require(hashlib.sha256(read_bytes(source / name)).hexdigest() == expected, 'Changed message source: ' + name)
    return observe_messages(manifest['inventory'], read, read(manifest['physics'])['m_LayerCollisionMatrix'])
