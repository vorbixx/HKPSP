"""Instantiate exported static spell-hit sprites in the active room bank."""
import math

from convert_room_visuals import decode, sprite_tint


def spell_emitter(reference, objects, read, ref, image, texture, ambient):
    root = reference['root']
    if root not in objects:
        return None
    parts = objects[root]['components']
    if not all(parts.get(k) for k in ('Transform', 'AutoRecycleSelf', 'RandomRotationRange')):
        return None
    root_transform = read(parts['Transform'][0])
    if any(root_transform['m_LocalScale']['m_' + a] != 1 for a in 'XYZ'):
        return None
    if any(not read(parts[k][0]).get('m_Enabled', True) for k in ('AutoRecycleSelf', 'RandomRotationRange')):
        return None
    sprites = [row for row in objects.values() if root in row['owners'] and
               row['components'].get('SpriteRenderer')]
    if len(sprites) != 1 or len(sprites[0]['owners']) != 2:
        return None
    row = sprites[0]
    if not read(row['go']).get('m_IsActive', True):
        return None
    transform = read(row['components']['Transform'][0])
    # This admission covers static billboard prefabs, not animated actors.
    if (set(row['components']) - {'GameObject', 'Transform', 'SpriteRenderer'} or
            any(transform['m_LocalPosition']['m_' + a] != 0 for a in 'XYZ') or
            any(transform['m_LocalRotation']['m_' + a] != 0 for a in 'XYZ')):
        return None
    recycle = read(parts['AutoRecycleSelf'][0])['m_Structure']
    rotation = read(parts['RandomRotationRange'][0])['m_Structure']
    life, low, high = recycle['timeToWait'], rotation['min'], rotation['max']
    scale = [transform['m_LocalScale']['m_' + a] for a in 'XY']
    if (recycle.get('afterEvent', 0) or recycle.get('isRealtime', False) or
            not all(math.isfinite(v) for v in (life, low, high, *scale)) or
            life <= 0 or life > 60 or high < low or min(scale) <= 0):
        raise ValueError('Invalid static spell-hit prefab: ' + root)
    renderer_key = row['components']['SpriteRenderer'][0]
    renderer = read(renderer_key)
    if not renderer.get('m_Enabled', True):
        return None
    if renderer.get('m_FlipX', False) or renderer.get('m_FlipY', False):
        return None
    sprite_key = ref(renderer_key, renderer['m_Sprite'])
    sprite = read(sprite_key)
    bitmap, bounds = decode(texture(ref(sprite_key, sprite['m_RD']['m_Texture'])), sprite)
    bounds = tuple(v * scale[i % 2] for i, v in enumerate(bounds))
    identifier = image(('spell-hit', root), bitmap, bounds)
    materials = [read(ref(renderer_key, p)) for p in renderer['m_Materials'] if p['m_PathID']]
    values = [0.] * 60
    values[7:9], values[11:13] = [life, life], [1., 1.]
    values[46:50] = sprite_tint(renderer['m_Color'], materials, ambient)
    # SpawnSpellHitEffect overwrites world depth after the prefab's OnEnable.
    values[2], values[9], values[10] = .0031, math.radians(low), math.radians(high)
    return dict(source=root, owner=root, sprite=identifier, tiles=1, count=1,
                rate=0, values=values, cone=[0.] * 11, bases=[0.] * 12,
                axes=[0.] * 18, shape_origins=[0.] * 9, shape=14, visual=True,
                sorting=[renderer['m_SortingLayer'], renderer['m_SortingOrder']])
