"""Read the saved Tutorial_01 grass SpriteAtlas slice for room conversion."""
import json
import base64
import math
import struct
from pathlib import Path


def path_id(value):
    return int(str(value).rsplit(':', 1)[1])


def packed_sprite(sprite, atlases):
    """Resolve Unity's SpriteAtlas override before reading pixels or geometry."""
    ref = sprite['m_SpriteAtlas']
    if ref['m_FileID'] != 0 or ref['m_PathID'] not in atlases:
        raise ValueError('Missing grass SpriteAtlas')
    key = sprite['m_RenderDataKey']
    entries = [value for candidate, value in atlases[ref['m_PathID']]['m_RenderDataMap']
               if candidate == key]
    if len(entries) != 1:
        raise ValueError('Missing or duplicate grass SpriteAtlas render key')
    rd = dict(sprite['m_RD'], **entries[0])
    if rd['texture']['m_FileID'] != 0 or rd['texture']['m_PathID'] <= 0:
        raise ValueError('Grass SpriteAtlas has no local texture')
    if rd['alphaTexture']['m_PathID'] or rd['downscaleMultiplier'] != 1:
        raise ValueError('Unsupported grass SpriteAtlas alpha or downscale')
    return dict(sprite, m_RD=rd)


def load(research):
    research = Path(research)
    inventory = json.loads((research / 'inventory.json').read_text())
    clips = json.loads((research / 'clip-summary.json').read_text())
    decoded = json.loads((research / 'decoded-selected.json').read_text())
    raw_path = research / 'prior' / 'raw-selected-objects.json'
    if not raw_path.is_file():
        raise ValueError('Missing saved raw grass Transform records')
    raw = json.loads(raw_path.read_text())
    if (inventory.get('build'), inventory.get('scene')) != (22529139, 'Tutorial_01'):
        raise ValueError('Unexpected grass research scene')
    push = {}
    for clip in clips['clips']:
        if not clip['name'].endswith('Push'):
            continue
        frames = [f for f in clip['frames'] if f['time'] < clip['stop']]
        if len(frames) != 4:
            raise ValueError(f"Expected four Push keys in {clip['name']}")
        push[clip['name']] = [path_id(f['sprite']) for f in frames]
    if len(push) != 5:
        raise ValueError(f'Expected five grass Push clips, found {len(push)}')
    sprites = {}
    atlases = {}
    # The object map keys, rather than Unity Sprite names, are the stable identity.
    for key, obj in decoded['objects'].items():
        if obj.get('type') == 'Sprite' and key.startswith('resources.assets:'):
            sprites[path_id(key)] = obj['data']
        if obj.get('type') == 'SpriteAtlas' and key.startswith('resources.assets:'):
            atlases[path_id(key)] = obj['data']
    stub_ids = {1931, 2258, 2353, 3558, 3623}
    wanted = set(stub_ids)
    for ids in push.values():
        wanted.update(ids)
    missing = wanted - sprites.keys()
    if missing:
        raise ValueError(f'Missing grass Sprite records: {sorted(missing)}')
    level6 = next((q['objects'] for q in raw['queries'] if q['label'] == 'level6'), None)
    if level6 is None:
        raise ValueError('Missing level6 grass transform query')
    particle_origins = {}
    for bush in inventory['owners']:
        particle_go = bush['cut_parameters']['particles']['m_PathID']
        game_object = level6.get(str(particle_go))
        if game_object is None or game_object.get('component') is None:
            raise ValueError(f'Missing particle child GameObject {particle_go}')
        transform_ids = [c['path_id'] for c in game_object['component']['components']]
        transforms = [level6.get(str(path)) for path in transform_ids]
        transform = next((item for item in transforms if item and item['class_id'] == 4), None)
        if transform is None or transform['size'] != 68:
            raise ValueError(f'Unsupported particle child Transform for owner {bush["owner"]}')
        origin = struct.unpack_from('<3f', bytes.fromhex(transform['hex']), 28)
        if not all(math.isfinite(value) for value in origin):
            raise ValueError(f'Invalid particle child origin for owner {bush["owner"]}')
        particle_origins[bush['owner']] = origin
    return inventory, push, {i: packed_sprite(sprites[i], atlases) for i in sorted(wanted)}, particle_origins


def decode(sprite, atlas):
    """Use the room decoder's tight-mesh alpha for the resolved atlas rectangle."""
    rd = sprite['m_RD']
    if rd['texture']['m_PathID'] <= 0:
        raise ValueError('Unresolved grass SpriteAtlas texture')
    rect = rd['textureRect']
    if (rect['x'] < 0 or rect['y'] < 0 or rect['width'] <= 0 or rect['height'] <= 0 or
            rect['x'] + rect['width'] > atlas.width or rect['y'] + rect['height'] > atlas.height):
        raise ValueError('Grass SpriteAtlas rectangle is outside its texture')
    # Research and room exports spell/encode the same Unity fields differently.
    names = dict(x='m_X', y='m_Y', width='m_Width', height='m_Height')
    vector = lambda value: {names[k]: v for k, v in value.items()}
    vd = rd['m_VertexData']
    normalized = dict(m_PixelsToUnits=sprite['m_PixelsToUnits'],
        m_Rect=vector(sprite['m_Rect']), m_Pivot=vector(sprite['m_Pivot']),
        m_RD=dict(m_TextureRect=vector(rect), m_TextureRectOffset=vector(rd['textureRectOffset']),
            m_SettingsRaw=rd['settingsRaw'],
            m_IndexBuffer=base64.b64encode(bytes(rd['m_IndexBuffer'])).decode('ascii'),
            m_VertexData=dict(m_VertexCount=vd['m_VertexCount'],
                m_Channels=[{f'm_{k.title()}': v for k, v in channel.items()} for channel in vd['m_Channels']],
                m_Data=base64.b64encode(bytes.fromhex(vd['m_DataSize']['hex'])).decode('ascii'))))
    from convert_room_visuals import decode as decode_room
    return decode_room(atlas, normalized)


def _affine(transform):
    p, s, q = transform['m_LocalPosition'], transform['m_LocalScale'], transform['m_LocalRotation']
    if abs(q['m_X']) > 5e-7 or abs(q['m_Y']) > 5e-7:
        raise ValueError('Grass whole/stub transform has unsupported 3D rotation')
    angle = 2 * math.atan2(q['m_Z'], q['m_W'])
    c, sn = math.cos(angle), math.sin(angle)
    values = (c*s['m_X'], -sn*s['m_Y'], p['m_X'],
              sn*s['m_X'], c*s['m_Y'], p['m_Y'])
    # Unity stores these fields as IEEE float32; compare the serialized matrix.
    return struct.unpack('<6f', struct.pack('<6f', *values))


def compile_bank(inventory, push, sprite_indices, draws, sprite_count, draw_count,
                 particle_origins):
    """Build the source-bound HKG1 table consumed by GrassBank."""
    import struct

    def sprite_index(path):
        indices = sprite_indices.get(path, [])
        if len(indices) != 1:
            raise ValueError(f'Grass sprite must use one atlas tile: {path}')
        return indices[0]

    def go_id(path):
        return int(path.rsplit('-', 1)[1].removesuffix('.json'))

    by_owner = {d['owner']: d for d in draws}
    records = []
    for bush in inventory['owners']:
        whole = next(r for r in bush['renderers'] if r['role'] == 'whole')
        stub = next(r for r in bush['renderers'] if r['role'] == 'stub')
        match = whole['sprite_name'].split('_', 1)[0]
        frames = push[match.title() + 'Push']
        owner_draw = go_id(whole['owner'])
        draw = by_owner.get(owner_draw)
        if draw is None or draw.get('grass') is None:
            raise ValueError(f"Missing whole grass draw for owner {bush['owner']}")
        collider = bush['colliders'][0]['data']
        if not collider['m_IsTrigger'] or not collider['m_Enabled']:
            raise ValueError(f"Grass trigger disabled for owner {bush['owner']}")
        x, y, z = bush['world_position']
        offset, size = collider['m_Offset'], collider['m_Size']
        sx, sy = bush['local_scale']['m_X'], bush['local_scale']['m_Y']
        corners = [(x + sx*(offset['m_X'] + dx*size['m_X']/2),
                    y + sy*(offset['m_Y'] + dy*size['m_Y']/2))
                   for dx, dy in ((-1,-1), (1,-1), (1,1), (-1,1))]
        left, right = min(p[0] for p in corners), max(p[0] for p in corners)
        bottom, top = min(p[1] for p in corners), max(p[1] for p in corners)
        scale_x = bush['local_scale']['m_X']
        if whole['local_transform']['m_Father'] != stub['local_transform']['m_Father']:
            raise ValueError(f'Grass whole/stub parent mismatch for owner {bush["owner"]}')
        whole_matrix, stub_matrix = _affine(whole['local_transform']), _affine(stub['local_transform'])
        if max(abs(a-b) for a,b in zip(whole_matrix,stub_matrix)) > 5e-7:
            raise ValueError(f'Grass whole/stub matrices differ for owner {bush["owner"]}')
        sprite_ref = stub['sprite_ref']
        if sprite_ref['m_FileID'] not in (0, 4):
            raise ValueError('Grass replacement Sprite uses an unexpected collection')
        stub_sprite = sprite_index(f"resources.assets-{sprite_ref['m_PathID']}.json")
        idle_sprite = sprite_index(whole['sprite_key'])
        push_sprites = [sprite_index(f'resources.assets-{value}.json') for value in frames]
        values = draw['values']
        sway = draw['grass']
        origin = particle_origins[bush['owner']]
        record = (bush['owner'], owner_draw, idle_sprite, *push_sprites, stub_sprite,
                  x, y, z, scale_x, left, bottom, right, top,
                  *values, sway['speed'], sway['amplitude'], sway['phase'],
                  sway['push'], sway['height'], *origin)
        if len(record) != 40:
            raise ValueError('Grass bank record layout mismatch')
        records.append(record)
    if len(records) != 41:
        raise ValueError(f'Expected 41 grass owners, found {len(records)}')
    binary = bytearray(struct.pack('<4s3I', b'HKG1', len(records), sprite_count, draw_count))
    for record in records:
        binary.extend(struct.pack('<8I32f', *record))
    return bytes(binary)
