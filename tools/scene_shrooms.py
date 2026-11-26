"""Original BounceShroom trigger circles (separate from solid terrain)."""
import math
import struct
import copy
import json
import re
from convert_terrain import transform_point


def export(mapping, read, target):
    circles = []
    for row in mapping['inventory']:
        local = row['components'][0]
        if 'BounceShroom' not in local:
            continue
        script = read(local['BounceShroom'][0])
        collider = read(row['collider'])
        if not (script['m_Enabled'] and script['m_Structure']['active'] and
                collider['m_Enabled'] and all(read(k)['m_IsActive'] for k in row['owners'])):
            continue
        if row['type'] != 'CircleCollider2D' or not collider['m_IsTrigger'] or read(row['go'])['m_Layer'] != 19:
            raise ValueError('Unsupported BounceShroom collider/layer')
        chain = [read(k) for k in row['chain']]
        offset = collider['m_Offset']
        center = transform_point([offset['m_X'], offset['m_Y']], chain)
        origin = transform_point([0, 0], chain)
        axes = [[a-b for a,b in zip(transform_point(p, chain), origin)] for p in ([1,0], [0,1])]
        if abs(sum(a*b for a,b in zip(*axes))) > 1e-4:
            raise ValueError('Sheared BounceShroom circle needs explicit support')
        radius = collider['m_Radius'] * max(math.hypot(*axis) for axis in axes)
        owner_id = script['m_GameObject']['m_PathID']
        collider_id = int(row['collider'].rsplit('-', 1)[1].split('.')[0])
        if not (0 < radius < 100 and all(math.isfinite(x) for x in (*center, radius))):
            raise ValueError('Invalid BounceShroom circle')
        circles.append(dict(owner=owner_id, collider=collider_id, center=center, radius=radius))
    if len(circles) > 256 or len({c['collider'] for c in circles}) != len(circles):
        raise ValueError('BounceShroom trigger capacity/identity')
    (target/'scene-shrooms.bin').write_bytes(struct.pack('<4sI', b'HSB1', len(circles)) + b''.join(
        struct.pack('<II3f', c['owner'], c['collider'], *c['center'], c['radius']) for c in circles))
    return circles


def artwork(mapping, source, read):
    """Reuse the authored draw/atlas, admitting original tk2d clip frames."""
    path = source/'tk2d/manifest.json'
    if not path.is_file():
        return [], {}
    supplement = json.loads(path.read_bytes())
    inventory = {r['go']: r for r in mapping['inventory']}
    profiles, sprites = [], {}
    for bouncer in supplement.get('bouncers', []):
        row = inventory[bouncer['owner']]
        script = read(row['components']['BounceShroom'][0])
        if not row['active'] or not script['m_Enabled'] or not script['m_Structure']['active']:
            continue
        initial = row['sprite']
        match = re.fullmatch(r'(.*)-sprite-(\d+)\.json', initial)
        if match is None:
            raise ValueError('Invalid BounceShroom sprite collection')
        collection_key = match[1]+'.json'
        definitions = read(collection_key)['m_Structure']['spriteDefinitions']
        material = definitions[int(match[2])]['materialId']
        clips = {c['name']: c for c in read(bouncer['library'])['m_Structure']['clips']}
        profile = dict(owner=int(bouncer['owner'].rsplit('-',1)[1].split('.')[0]), initial=initial, clips=[])
        for state in ('idle', 'bounce'):
            clip = clips[bouncer[state]]
            # tk2d WrapMode Single=6, Once=2. Other policies need runtime support.
            mode = 6 if state == 'idle' else 2
            if not (clip['wrapMode'] == mode and 0 < clip['fps'] <= 120 and
                    0 < len(clip['frames']) <= 64 and (state != 'idle' or len(clip['frames']) == 1)):
                raise ValueError('Unsupported BounceShroom clip')
            keys = []
            for frame in clip['frames']:
                ref = frame['spriteCollection']
                if ref['m_FileID'] != 0 or not collection_key.endswith('-'+str(ref['m_PathID'])+'.json'):
                    raise ValueError('External BounceShroom frame collection is unresolved')
                definition = copy.deepcopy(definitions[frame['spriteId']])
                if definition['complexGeometry'] or len(definition['positions']) != 4 or len(definition['uvs']) != 4 or definition['materialId'] != material:
                    raise ValueError('Unsupported BounceShroom sprite geometry/material')
                definition['materialId'] = 0
                key = match[1]+'-sprite-'+str(frame['spriteId'])+'.json'
                sprites[key] = (source/mapping['sprites'][initial], dict(_tk2d_definition=definition, m_PixelsToUnits=1))
                keys.append(key)
            profile['clips'].append(dict(fps=clip['fps'], frames=keys))
        profiles.append(profile)
    return profiles, sprites


def bind_artwork(profiles, by_sprite, draws):
    data = bytearray(struct.pack('<4sI', b'HSA1', len(profiles)))
    for profile in profiles:
        selected = [d for d in draws if d['owner'] == profile['owner']]
        if len(selected) != 1 or by_sprite[profile['initial']] != [selected[0]['sprite']]:
            raise ValueError('BounceShroom authored draw missing or split')
        data.extend(struct.pack('<II', profile['owner'], selected[0]['sprite']))
        for clip in profile['clips']:
            indices = [by_sprite[key] for key in clip['frames']]
            if any(len(tiles) != 1 for tiles in indices):
                raise ValueError('BounceShroom animation frame split')
            data.extend(struct.pack('<fI', clip['fps'], len(indices)))
            data.extend(struct.pack('<'+'I'*len(indices), *(tiles[0] for tiles in indices)))
    return data


def resident_artwork(binary, source, resident):
    """Resident packing keeps the sprite IDs referenced by HSA1 clips."""
    if len(binary) < 8 or binary[:4] != b'HSA1':
        raise ValueError('Invalid source shroom artwork bank')
    if len(source) < 20 or len(resident) < 20:
        raise ValueError('Invalid shroom room metadata')
    sprites, = struct.unpack_from('<I', source, 8)
    if (source[8:16] != resident[8:16] or
            source[20+sprites*36:] != resident[20+sprites*36:]):
        raise ValueError('Resident room changed shroom sprite or draw identity')
    return binary
