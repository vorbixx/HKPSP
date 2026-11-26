"""Bounded external SpatterOrange spawns, including registered Breakable.FlingObject bursts."""
import math

from convert_room_visuals import decode, sprite_tint


def fling_emitter(reference, objects, read, ref, image, texture, ambient):
    root = reference['root']
    parts = objects[root]['components']
    if not all(parts.get(k) for k in ('SpatterOrange', 'SpriteRenderer', 'Rigidbody2D', 'CircleCollider2D')):
        return None
    data = read(parts['SpatterOrange'][0])['m_Structure']
    controller = read(reference['component'])['m_Structure']
    # Breakable uses the first register entry matching the chosen prefab. Without
    # one it spawns a single object at rest, not a constructor-default burst.
    launch = None
    for entry in controller.get('flingObjectRegister', []) or []:
        pointer = entry.get('referenceObject', {})
        if pointer.get('m_PathID') and ref(reference['component'], pointer) == root:
            launch = entry
            break
    if launch is None:
        launch = dict(spawnMin=1, spawnMax=1, speedMin=0, speedMax=0,
                      angleMin=0, angleMax=0, originVariation=dict(x=0, y=0))
    # Omitted registered fields retain the nested FlingObject constructor defaults.
    minimum, maximum = launch.get('spawnMin', 25), launch.get('spawnMax', 30)
    speed = [launch.get('speedMin', 9), launch.get('speedMax', 20)]
    angle = [launch.get('angleMin', 20), launch.get('angleMax', 160)]
    variation = launch.get('originVariation', dict(x=.5, y=.5))
    variation = [variation.get('x', .5), variation.get('y', .5)]
    fps = data.get('fps', 30)
    splash = [data.get('splashScaleMin', 1.5), data.get('splashScaleMax', 2)]
    size = [data.get('scaleModifierMin', .7), data.get('scaleModifierMax', 1.3)]
    values_to_check = [*speed, *angle, *variation, fps, *splash, *size]
    if (not isinstance(minimum, int) or not isinstance(maximum, int) or
            not 0 <= minimum <= maximum <= 256 or fps <= 0 or fps > 240 or
            speed[0] < 0 or speed[1] < speed[0] or angle[1] < angle[0] or
            not all(math.isfinite(v) and abs(v) <= 1e6 for v in values_to_check) or
            min(*variation, *size, *splash) < 0 or min(*size, *splash) == 0):
        raise ValueError('Invalid breakable fling parameters: ' + root)
    renderer_key = parts['SpriteRenderer'][0]
    renderer = read(renderer_key)
    sprites = data['sprites']
    if len(sprites) != 7:
        raise ValueError('SpatterOrange expects seven animation frames: ' + root)
    first = None
    for number, pointer in enumerate(sprites):
        key = ref(parts['SpatterOrange'][0], pointer)
        sprite = read(key)
        atlas = texture(ref(key, sprite['m_RD']['m_Texture']))
        bitmap, bounds = decode(atlas, sprite)
        identifier = image(('fling', root, number), bitmap, bounds)
        if first is None:
            first = identifier
        if identifier != first + number:
            raise ValueError('Noncontiguous fling sprite sequence: ' + root)
    body = read(parts['Rigidbody2D'][0])
    mass = body.get('m_Mass', 1)
    if not math.isfinite(mass) or mass <= 0:
        raise ValueError('Invalid fling body mass: ' + root)
    collider = read(parts['CircleCollider2D'][0])
    v = [0.] * 60
    v[3:5] = [math.radians(sum(angle) / 2), math.radians(angle[1] - angle[0])]
    # SpatterOrange.Update invokes Impact after three seconds in flight.
    v[7:9] = [3 + len(sprites) / fps] * 2
    v[9:11] = [value / mass for value in speed]  # FlingObject applies impulse.
    # ProjectileSquash replaces local XY scale with squash * scaleModifier.
    v[11:13] = sorted(size)
    v[15], v[19] = body.get('m_GravityScale', 1), 1e6
    v[22:38] = [1.] * 16
    materials = [read(ref(renderer_key, p)) for p in renderer['m_Materials'] if p['m_PathID']]
    v[46:50] = sprite_tint(renderer['m_Color'], materials, ambient)
    v[59] = 2 * collider['m_Radius']
    return dict(source=root, owner=root, sprite=first, tiles=7, count=maximum,
                rate=0, values=v, cone=[0.] * 11, bases=[0.] * 12,
                axes=[0.] * 18, shape_origins=[0.] * 9, shape=13, visual=False,
                sorting=[renderer['m_SortingLayer'], renderer['m_SortingOrder']],
                fling=[minimum, *variation, fps, *sorted(splash)])
