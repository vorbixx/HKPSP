"""Source-derived Great Door body settings and unsampled dust alpha gradients."""
import hashlib
import json
import math


def source_data(root, shapes=None):
    source = root / '_private/research/great-door-motion-source'
    mapping = json.loads((source / 'motion-source-map.json').read_bytes())
    if mapping['build_id'] != 22529139:
        raise ValueError('Wrong Great Door motion source build')
    for name, sha in mapping['sha256'].items():
        if hashlib.sha256((source / name).read_bytes()).hexdigest() != sha:
            raise ValueError('Changed Great Door motion source: ' + name)
    def read(name):
        raw = json.loads((source / name).read_bytes())
        return raw.get('m_Structure', raw)
    timestep = read(mapping['time'])['m_Fixed_Timestep']
    fixed_dt = timestep['m_Count'] / (timestep['m_Rate']['m_Numerator'] / timestep['m_Rate']['m_Denominator'])
    from inspect_fsm import decode_fsm
    fsm_raw = json.loads((root / '_private/research/great-door/level6-12139.json').read_bytes())
    state = next(s for s in decode_fsm(fsm_raw['m_Structure']['fsm'])['states'] if s['name'] == 'Break 2')
    spawns = [a['parameters'] for a in state['actions']
              if a['enabled'] and a['type'].endswith('.SpawnRandomObjectsV2')]
    bodies = []
    for owner, spawn in zip(('505', '482'), spawns):
        if spawn['gameObject']['value']['m_PathID'] != int(owner):
            raise ValueError('Changed Great Door shard action order')
        parts = mapping['objects'][owner]
        def component(kind):
            return read(parts[kind][0])
        body, polygon = component('Rigidbody2D'), component('PolygonCollider2D')
        scale = component('Transform')['m_LocalScale']
        paths = polygon['m_Points']['m_Paths']
        if len(paths) != 1 or polygon['m_IsTrigger']:
            raise ValueError('Expected one solid shard polygon')
        vertices = [(v['m_X'] * scale['m_X'], v['m_Y'] * scale['m_Y']) for v in paths[0]]
        area2 = cx = cy = moment = 0.
        for (x, y), (u, v) in zip(vertices, vertices[1:] + vertices[:1]):
            cross = x*v-u*y
            area2 += cross; cx += (x+u)*cross; cy += (y+v)*cross
            moment += cross*(x*x+x*u+u*u+y*y+y*v+v*v)
        inertia = body['m_Mass']*(moment/(6*area2)-(cx/(3*area2))**2-(cy/(3*area2))**2)
        if shapes is not None:
            if len(vertices) > 7 or any(polygon['m_Offset'].values()):
                raise ValueError('Unsupported shard polygon')
            shapes.append([len(vertices), cx/(3*area2), cy/(3*area2)] +
                          [c for point in vertices for c in point] + [0.]*(14-2*len(vertices)))
        position = spawn['position']['value']
        for name in ('speed', 'angle'):
            if spawn[name+'Min']['value'] != spawn[name+'Max']['value']:
                raise ValueError('Expected constant shard ' + name)
        bounce = component('ObjectBounce')
        bodies.append([position[a] for a in 'xyz'] + [spawn['speedMin']['value'],
            math.radians(spawn['angleMin']['value']), component('SpinSelf')['spinFactor'],
            body['m_AngularDamping'], body['m_LinearDamping'], body['m_Mass'], inertia,
            body['m_GravityScale'], component('PhysicsMaterial2D')['m_Friction'],
            bounce['bounceFactor'], bounce['speedThreshold'],
            max(math.hypot(x,y) for x,y in vertices), fixed_dt])
    return bodies


def alpha_keys(module):
    gradient = module['m_Gradient']
    mode = gradient['m_MinMaxState']
    if not module['m_Enabled'] or mode not in (1, 3):
        raise ValueError('Unsupported Great Door alpha gradient')
    result = []
    for name in ('m_MinGradient', 'm_MaxGradient'):
        raw = gradient[name if mode == 3 else 'm_MaxGradient']
        keys = sorted((raw[f'm_Atime{i}']/65535, raw[f'm_Key{i}']['m_A'])
                      for i in range(raw['m_NumAlphaKeys']))
        if len(keys) != 4:
            raise ValueError('Expected four Great Door alpha keys')
        result += [v for key in keys for v in key]
    return result
