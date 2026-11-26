"""Compile activated SpriteRenderer debris; particle prefabs use the effects bank."""
import math


def components(rows, owner):
    result = {}
    for row in rows:
        groups = row.get('components', {})
        if isinstance(groups, list):
            groups = {kind: keys for group in groups if owner in group.get('GameObject', [])
                      for kind, keys in group.items()}
        result.update(groups)
    return result


def launch(data, kind):
    """Right, up, left, down, in degrees and world units/second (or impulse)."""
    if kind == 'BreakableObject':
        return [value for direction in ('right', 'up', 'left', 'down')
                for field, default in (('minFlingAngle', 5), ('maxFlingAngle', 5),
                                       ('minFlingSpeed', 4), ('maxFlingSpeed', 4))
                for value in [data.get(direction, {}).get(field, default)]]
    lo, hi = data.get('flingSpeedMin', 10), data.get('flingSpeedMax', 17)
    return [v for amin, amax, scale in ((30, 70, 1), (70, 110, 1.5), (120, 160, 1), (160, 380, 1))
            for v in (amin, amax, lo * scale, hi * scale)]


def compile_debris(data, kind, inventory, read, resolve, project):
    pieces, unresolved = [], []
    for ref in data.get('debrisParts' if kind == 'Breakable' else 'flingDebris', []):
        root = resolve(None, ref)
        if root is None:
            continue  # Null entries are skipped by the source activation loop.
        rows = inventory.get(root, [])
        parts = components(rows, root)
        renderers = [row for rr in inventory.values() for row in rr
                     if row.get('sprite') and root in row.get('owners', [])
                     and read(row['renderer']).get('m_Enabled', True)]
        # ParticleSystems and unexported dependencies cannot be interpreted as bodies.
        if not rows or not renderers:
            unresolved.append(root)
            continue
        row = rows[0]
        chain = [read(key) for key in row['chain']]
        origin = project((0, 0, 0), chain)
        body = read(parts['Rigidbody2D'][0]) if parts.get('Rigidbody2D') else {}
        dynamic = bool(body and body.get('m_Simulated', True) and body.get('m_BodyType', 0) == 0)
        points, material = [], {}
        for shape_kind in ('BoxCollider2D', 'PolygonCollider2D', 'CircleCollider2D'):
            for key in parts.get(shape_kind, []):
                shape = read(key)
                if not shape.get('m_Enabled', True) or shape.get('m_IsTrigger', False):
                    continue
                offset = shape['m_Offset']
                if shape_kind == 'BoxCollider2D':
                    size = shape['m_Size']
                    paths = [[(x * size['m_X'], y * size['m_Y']) for x, y in
                              ((-.5, -.5), (.5, -.5), (.5, .5), (-.5, .5))]]
                elif shape_kind == 'CircleCollider2D':
                    # Seven vertices fit the shared bounded convex-body solver.
                    radius = shape['m_Radius']
                    paths = [[(radius*math.cos(i*2*math.pi/7), radius*math.sin(i*2*math.pi/7))
                              for i in range(7)]]
                else:
                    paths = [[(p['m_X'], p['m_Y']) for p in path] for path in shape['m_Points']['m_Paths']]
                points.extend((p[0]-origin[0], p[1]-origin[1]) for path in paths for x, y in path
                              for p in [project((x+offset['m_X'], y+offset['m_Y'], 0), chain)])
                material_key = resolve(key, shape.get('m_Material', {}))
                if material_key:
                    try:
                        material = read(material_key)
                    except KeyError:
                        pass  # Older exports did not retain PhysicsMaterial2D.
        # The shared decorative solver uses one convex hull, at most seven vertices.
        # Nonconvex/compound debris contacts are a documented physics approximation.
        hull_points = convex_hull(points)
        if len(hull_points) > 7:
            xs, ys = zip(*hull_points)
            hull_points = [(min(xs), min(ys)), (max(xs), min(ys)), (max(xs), max(ys)), (min(xs), max(ys))]
        center, inertia = [0., 0.], .0001
        mass = body.get('m_Mass', 1)
        if hull_points:
            area2 = cx = cy = moment = 0.
            for (x, y), (u, v) in zip(hull_points, hull_points[1:] + hull_points[:1]):
                cross = x*v-u*y
                area2 += cross; cx += (x+u)*cross; cy += (y+v)*cross
                moment += cross*(x*x+x*u+u*u+y*y+y*v+v*v)
            if abs(area2) > 1e-8:
                center = [cx/(3*area2), cy/(3*area2)]
                inertia = max(.0001, mass*(moment/(6*area2)-sum(c*c for c in center)))
            else:
                hull_points = []
        radius = max((math.hypot(*p) for p in hull_points), default=.01)
        bounce = read(parts['ObjectBounce'][0])['m_Structure'] if parts.get('ObjectBounce') else {}
        finish_component = read(parts['FinishingRigidBody'][0]) if parts.get('FinishingRigidBody') else {}
        finishing = finish_component.get('m_Structure', {}) if finish_component.get('m_Enabled', True) else {}
        spin = read(parts['SpinSelf'][0])['m_Structure'].get('spinFactor', 0) if parts.get('SpinSelf') else 0
        rotation = [0., 0.]
        if parts.get('SpinSelfSimple'):
            component = read(parts['SpinSelfSimple'][0])
            simple = component.get('m_Structure', {}) if component.get('m_Enabled', True) else {}
            # OnEnable waits .01 s; DoSpin adds force torque -velocity.x * spinFactor.
            # waitForCall instances require an authored caller and do not self-start.
            spin = 0 if simple.get('waitForCall', False) else -simple.get('spinFactor', 0)
            if simple.get('randomStartRotation', False):
                q = chain[0]['m_LocalRotation']
                angle = math.atan2(2*(q['m_W']*q['m_Z']+q['m_X']*q['m_Y']),
                                   1-2*(q['m_Y']**2+q['m_Z']**2))
                parent = chain[1:]
                base, x, y = (project(p, parent) for p in ((0,0,0),(1,0,0),(0,1,0)))
                sign = -1 if (x[0]-base[0])*(y[1]-base[1])-(x[1]-base[1])*(y[0]-base[0]) < 0 else 1
                rotation = sorted((-sign*angle, sign*(2*math.pi-angle)))
        physics = [*origin, *rotation, spin, body.get('m_AngularDamping', .05),
                   body.get('m_LinearDamping', 0), mass, inertia,
                   body.get('m_GravityScale', 1), material.get('m_Friction', .4),
                   bounce.get('bounceFactor', material.get('m_Bounciness', 0)), bounce.get('speedThreshold', 1), radius, .02]
        hull = [len(hull_points), *center, *(v for p in hull_points for v in p)]
        hull += [0] * (17-len(hull))
        owners = sorted({int(r['go'].rsplit('-', 1)[1][:-5]) for r in renderers})
        pieces.append(dict(root=root, owners=owners, flags=int(dynamic) | (2 if finishing else 0) |
                           (4 if finishing.get('persistOffScreen', False) else 0),
                           physics=physics, hull=hull, finish=[finishing.get('waitDuration', 0),
                                                             finishing.get('shrinkDuration', 0)]))
    return pieces, unresolved


def convex_hull(points):
    points = sorted(set(points))
    if len(points) <= 2:
        return points
    def cross(a, b, c):
        return (b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0])
    def half(sequence):
        result = []
        for p in sequence:
            while len(result) > 1 and cross(result[-2], result[-1], p) <= 0:
                result.pop()
            result.append(p)
        return result[:-1]
    return half(points)+half(reversed(points))
