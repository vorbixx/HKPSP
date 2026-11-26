"""Confirmed permanent pickups; keep this slice separate from Geo/ability ownership.

Build 22529139 Bottle Control: Idle NAIL HIT -> Shatter increments
grubsCollected and sends FREE. The persistent owner is the bottle, not its
Grub child. Unknown rewards and unreviewed rooms stay outside this table.
"""
import math
import struct

SCENES = {'Crossroads_03': 39, 'Crossroads_05': 41, 'Fungus1_06': 135,
          'Fungus1_13': 142, 'Fungus1_21': 149, 'Fungus1_28': 155, 'Waterways_13': 362}
GRUB_BOTTLES = {39: (428,), 41: (103,), 135: (1701,), 142: (1436,),
                149: (539,), 155: (418,), 362: (46,)}
HERO_RANGE_TOKENS = (b'Trigger2dEvent', b'ENTER', b'EXIT', b'SetFsmBool',
                     b'Bottle Control', b'Hero Range')


def hero_range_bounds(asset, bottle_transform):
    candidates = []
    for owner in asset.descendants(bottle_transform):
        go = asset.gos[owner]
        transform = asset.transform(owner)
        if go['name'] == 'Hero Range' and asset.active(transform):
            candidates.append((owner, transform, go))
    if len(candidates) != 1:
        raise ValueError('Expected one active Hero Range descendant')
    owner, transform, go = candidates[0]

    fsms = [asset.objects[cid][1] for cid in go['components']
            if asset.objects[cid][0] == 114 and
            all(token in asset.objects[cid][1] for token in HERO_RANGE_TOKENS)]
    if len(fsms) != 1:
        raise ValueError('Unconfirmed Hero Range FSM')

    colliders = [asset.objects[cid][1] for cid in go['components']
                 if asset.objects[cid][0] == 61 and asset.objects[cid][1][12]]
    if len(colliders) != 1:
        raise ValueError('Expected one enabled Hero Range BoxCollider2D')
    collider = colliders[0]
    if len(collider) != 148:
        raise ValueError('Unsupported Hero Range BoxCollider2D layout')
    x, y = struct.unpack_from('<2f', collider, 72)
    width, height = struct.unpack_from('<2f', collider, 136)
    if width <= 0 or height <= 0:
        raise ValueError('Invalid Hero Range collider')
    points = [asset.point(transform, (x+dx*width/2, y+dy*height/2))
              for dx in (-1, 1) for dy in (-1, 1)]
    bounds = [min(p[0] for p in points), min(p[1] for p in points),
              max(p[0] for p in points), max(p[1] for p in points)]
    if not all(math.isfinite(v) for v in bounds):
        raise ValueError('Nonfinite Hero Range bounds')
    return bounds


def placements(asset, scene_number):
    records = []
    for owner in GRUB_BOTTLES.get(scene_number, ()):
        go = asset.gos[owner]
        if go['name'] != 'Grub Bottle':
            raise ValueError('Changed source grub bottle identity')
        transform = asset.transform(owner)
        if not asset.active(transform):
            continue
        fsms = [(p, asset.objects[p][1]) for p in go['components']
                if asset.objects[p][0] == 114]
        bottle = [(p, data) for p, data in fsms
                  if all(token in data for token in
                         (b'Bottle Control', b'NAIL HIT', b'Shatter', b'grubsCollected', b'FREE'))]
        if len(bottle) != 1:
            raise ValueError('Unconfirmed grub acquisition FSM')
        points = []
        for component in go['components']:
            kind, data = asset.objects[component]
            if kind != 61 or not data[12]:
                continue
            if len(data) != 148:
                raise ValueError('Unsupported grub BoxCollider2D layout')
            x, y = struct.unpack_from('<2f', data, 72)
            width, height = struct.unpack_from('<2f', data, 136)
            if width <= 0 or height <= 0:
                raise ValueError('Invalid grub bottle collider')
            points.extend(asset.point(transform, (x+dx*width/2, y+dy*height/2))
                          for dx in (-1, 1) for dy in (-1, 1))
        if not points:
            raise ValueError('Missing grub bottle collider')
        position = asset.point(transform, (0, 0))
        bounds = [min(p[0] for p in points), min(p[1] for p in points),
                  max(p[0] for p in points), max(p[1] for p in points)]
        if not all(math.isfinite(v) for v in (*position, *bounds)):
            raise ValueError('Nonfinite grub placement')
        hero_range = hero_range_bounds(asset, transform)
        records.append(dict(id=scene_number*1000000+owner, owner=owner, kind=0,
                            hits=1, value=1, position=position, bounds=bounds,
                            require_hero_range=True, hero_range=hero_range,
                            visual_owners=asset.descendants(transform), name=go['name'],
                            source_fsm=bottle[0][0], acquisition='NAIL HIT -> Shatter: grubsCollected += 1'))
    return records
