"""Prepare Tutorial_01 stalactite sprite templates from verified room exports."""
import hashlib
import json
import math
import shutil
import struct
from pathlib import Path

from PIL import Image

from convert_room_visuals import decode

ROOT = Path(__file__).resolve().parents[1]
TUTORIAL = ROOT / '_private/research/tutorial'
ROOM = ROOT / '_private/research/room-visuals'
CONTROLS = (12643, 12644, 12645, 12646, 12639, 12640, 12641, 12642)


def _hash(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _matrix(chain, read):
    # Unity's exported room transforms are planar. Keep the affine transform in
    # XY plus an independent Z scale, and reject rotations outside that model.
    a, b, c, d, tx, ty, sz, tz = 1., 0., 0., 1., 0., 0., 1., 0.
    for key in reversed(chain):
        t = read(key)
        q = t['m_LocalRotation']
        if abs(q['m_X']) > 1e-6 or abs(q['m_Y']) > 1e-6:
            raise ValueError('Stalactite artwork has nonplanar rotation')
        angle = 2 * math.atan2(q['m_Z'], q['m_W'])
        co, si = math.cos(angle), math.sin(angle)
        s, p = t['m_LocalScale'], t['m_LocalPosition']
        la, lb, lc, ld = co*s['m_X'], si*s['m_X'], -si*s['m_Y'], co*s['m_Y']
        a, b, c, d, tx, ty = (
            a*la+c*lb, b*la+d*lb, a*lc+c*ld, b*lc+d*ld,
            a*p['m_X']+c*p['m_Y']+tx, b*p['m_X']+d*p['m_Y']+ty)
        tz += sz*p['m_Z']
        sz *= s['m_Z']
    return a, b, c, d, tx, ty, sz, tz


def _relative(root, child):
    a, b, c, d, tx, ty, sz, tz = root
    det = a*d-b*c
    if abs(det) < 1e-8 or abs(sz) < 1e-8:
        raise ValueError('Stalactite root transform is singular')
    ia, ib, ic, id_ = d/det, -b/det, -c/det, a/det
    x, y = child[4]-tx, child[5]-ty
    return (ia*child[0]+ic*child[1], ib*child[0]+id_*child[1],
            ia*child[2]+ic*child[3], ib*child[2]+id_*child[3],
            ia*x+ic*y, ib*x+id_*y, child[6]/sz, (child[7]-tz)/sz)


def _renderer(go, read):
    transforms = []
    renderer = None
    for component in go['m_Components']:
        path_id = component['m_Component']['m_PathID']
        obj = read(f'level6-{path_id}.json')
        if 'm_LocalPosition' in obj:
            transforms.append(path_id)
        if obj.get('m_Sprite', {}).get('m_PathID', 0):
            if renderer is not None:
                raise ValueError('Expected one stalactite SpriteRenderer')
            renderer = obj
    if len(transforms) != 1 or renderer is None:
        raise ValueError('Missing stalactite transform or SpriteRenderer')
    return renderer, transforms[0]


def _template(renderer, matrix):
    a, b, c, d, tx, ty, sz, tz = matrix
    if renderer['m_FlipX']:
        a, b = -a, -b
    if renderer['m_FlipY']:
        c, d = -c, -d
    color = renderer['m_Color']
    return [a, b, tx, c, d, ty, tz,
            color['m_R'], color['m_G'], color['m_B'], color['m_A'],
            renderer['m_SortingLayer'], renderer['m_SortingOrder'], 0., 0., 0.]


def prepare(data, output, romfs):
    """Append source sprites and write the 8-record STV1 sprite-template bank."""
    output, romfs = Path(output).absolute(), Path(romfs).absolute()
    private = (ROOT / '_private').absolute()
    if not output.is_relative_to(private) or not romfs.is_relative_to(private):
        raise ValueError('Stalactite artwork outputs must remain private')
    mapping_path = ROOM / 'room-visuals-map.json'
    mapping_raw = mapping_path.read_bytes()
    mapping = json.loads(mapping_raw)
    source_hashes = {'_private/research/room-visuals/room-visuals-map.json':
                     hashlib.sha256(mapping_raw).hexdigest()}
    inventory_path = TUTORIAL / 'inventory.json'
    source_hashes['_private/research/tutorial/inventory.json'] = _hash(inventory_path)
    verified = mapping['source_sha256']
    cache = {}

    def read_tutorial(name):
        if name not in cache:
            path = TUTORIAL / name
            raw = path.read_bytes()
            expected = json.loads((TUTORIAL / 'inventory.json').read_text())['source_sha256'].get(name)
            if expected and hashlib.sha256(raw).hexdigest() != expected:
                raise ValueError('Changed Tutorial source: ' + name)
            source_hashes[str(path.relative_to(ROOT)).replace('\\', '/')]=hashlib.sha256(raw).hexdigest()
            cache[name] = json.loads(raw)
        return cache[name]

    room_cache = {}

    def read_room(name):
        if name not in room_cache:
            path = ROOM / name
            raw = path.read_bytes()
            if verified.get(name) != hashlib.sha256(raw).hexdigest():
                raise ValueError('Changed room visual source: ' + name)
            source_hashes[str(path.relative_to(ROOT)).replace('\\', '/')]=hashlib.sha256(raw).hexdigest()
            room_cache[name] = json.loads(raw)
        return room_cache[name]

    artwork = {}
    image_ids = {}
    records = []
    for ordinal, control_id in enumerate(CONTROLS):
        control_name = f'level6-{control_id}.json'
        control = read_tutorial(control_name)['m_Structure']
        root_id = read_tutorial(control_name)['m_GameObject']['m_PathID']
        root_go = read_tutorial(f'level6-{root_id}.json')
        embedded_go = read_tutorial(f"level6-{control['embeddedVersion']['m_PathID']}.json")
        body_renderer, body_transform = _renderer(root_go, read_tutorial)
        embedded_renderer, embedded_transform = _renderer(embedded_go, read_tutorial)
        body_key = f"resources.assets-{body_renderer['m_Sprite']['m_PathID']}.json"
        embedded_key = f"resources.assets-{embedded_renderer['m_Sprite']['m_PathID']}.json"
        if body_key != embedded_key:
            raise ValueError('Embedded stalactite uses a different source Sprite')
        sprite_key = body_key
        if sprite_key not in mapping['sprites']:
            raise ValueError('Missing stalactite Sprite from room visual export: ' + sprite_key)
        if sprite_key not in image_ids:
            sprite = read_room(sprite_key)
            texture_name = mapping['sprites'][sprite_key]
            texture_path = ROOM / texture_name
            if verified.get(texture_name) != _hash(texture_path):
                raise ValueError('Changed stalactite atlas texture: ' + texture_name)
            source_hashes[str(texture_path.relative_to(ROOT)).replace('\\', '/')]=_hash(texture_path)
            bitmap, bounds = decode(Image.open(texture_path).convert('RGBA'), sprite)
            image_ids[sprite_key] = len(data['images'])
            data['images'].append(bitmap)
            data['geometry'].append(bounds)

        def ancestors(transform_id):
            chain=[];seen=set()
            while transform_id:
                if transform_id in seen:
                    raise ValueError('Cyclic stalactite transform hierarchy')
                seen.add(transform_id)
                key=f'level6-{transform_id}.json';chain.append(key)
                transform_id=read_tutorial(key)['m_Father']['m_PathID']
            return chain

        body_chain=ancestors(body_transform)
        embedded_chain=ancestors(embedded_transform)
        root_matrix=_matrix(body_chain,read_tutorial)
        embedded_matrix=_relative(root_matrix,_matrix(embedded_chain,read_tutorial))
        # XY follows the actor spawn, while source depth remains the scene-authored Z.
        root_transform=read_tutorial(f'level6-{body_transform}.json')
        q=root_transform['m_LocalRotation']; scale=root_transform['m_LocalScale']
        angle=2*math.atan2(q['m_Z'],q['m_W']); co,si=math.cos(angle),math.sin(angle)
        body_matrix=(co*scale['m_X'],si*scale['m_X'],-si*scale['m_Y'],co*scale['m_Y'],
                     0.,0.,root_matrix[6],root_matrix[7])
        embedded_matrix=(*embedded_matrix[:7],_matrix(embedded_chain,read_tutorial)[7])
        body_values=_template(body_renderer,body_matrix)
        embedded_values=_template(embedded_renderer,embedded_matrix)
        records.append((root_id,image_ids[sprite_key],body_values,
                        image_ids[sprite_key],embedded_values))

    binary=bytearray(struct.pack('<4sI',b'STV1',len(records)))
    for owner,body_id,body_values,embedded_id,embedded_values in records:
        binary.extend(struct.pack('<I',owner))
        binary.extend(struct.pack('<I16f',body_id,*body_values))
        binary.extend(struct.pack('<I16f',embedded_id,*embedded_values))
    output.mkdir(parents=True,exist_ok=True);romfs.mkdir(parents=True,exist_ok=True)
    (output/'stalactite-visuals.bin').write_bytes(binary)
    shutil.copyfile(output/'stalactite-visuals.bin',romfs/'stalactite-visuals.bin')
    data['stalactite_source_sha256']=source_hashes
    data['stalactite_artwork']={'records':len(records),'sprite':'resources.assets-2582.json',
        'owner_order':[record[0] for record in records],'binary':'stalactite-visuals.bin'}
    return records
