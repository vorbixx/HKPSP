"""Original Mantis gate/lever sprites and one-shot source timelines."""
import hashlib
import json
import struct
from pathlib import Path
import sys


def add_tiles(source, mapping, tiles):
    if mapping['scene'] != 'Fungus2_15':
        return [], {}
    from assetripper_client import ROOT
    from convert_hero import decode_sprite
    from unity_transform import project
    for folder in ('_private/tools/boss-python', '_private/converted/route-missing-rooms/python'):
        packages = ROOT / folder
        try:
            available = (packages / 'UnityPy/__init__.py').is_file()
        except PermissionError:
            available = False
        if available:
            sys.path.insert(0, str(packages))
            break
    import UnityPy
    from watcher_unity import AlignedGenerator
    original = ROOT / '_private/original/Hollow Knight/hollow_knight_Data'
    env = UnityPy.Environment(path=str(original))
    generator = AlignedGenerator('6000.0.61f1')
    generator.load_local_dll_folder(str(original / 'Managed'))
    env.typetree_generator = generator
    files, hashes = {}, {}

    def load(name):
        if name not in files:
            path = original / name
            hashes[name] = hashlib.sha256(path.read_bytes()).hexdigest()
            env.load_file(str(path))
            files[name] = next(f for f in env.files.values() if getattr(f, 'name', None) == name)
        return files[name]

    def resolve(owner, ref):
        name = owner.name if ref['m_FileID'] == 0 else Path(owner.externals[ref['m_FileID'] - 1].path).name
        return load(name).objects[ref['m_PathID']]

    terrain = source.parent / 'terrain'
    read = lambda name: json.loads((terrain / name).read_text())
    terrain_map = read('terrain-map.json')
    scene = load('level179')
    decoded, draws, animations = {}, [], []
    selected = [r for r in mapping['mesh_renderer_audit']
                if r['name'].startswith(('Mantis Gate', 'Mantis Lever'))]
    if len(selected) != 10:
        raise ValueError('Expected five Mantis gates and five levers')
    def parts_for(collection_obj, sprite_id):
        key = (collection_obj.assets_file.name, collection_obj.path_id, sprite_id)
        if key not in decoded:
            collection = collection_obj.read_typetree()
            definition = collection['spriteDefinitions'][sprite_id]
            material_obj = resolve(collection_obj.assets_file, collection['materials'][definition['materialId']])
            material = material_obj.read_typetree()
            texture = dict(material['m_SavedProperties']['m_TexEnvs'])['_MainTex']['m_Texture']
            atlas = resolve(material_obj.assets_file, texture).read().image.convert('RGBA')
            image, (left, bottom, right, top) = decode_sprite(atlas, dict(definition, materialId=0))
            parts = []
            for y in range(0, image.height, 512):
                for x in range(0, image.width, 512):
                    bitmap = image.crop((x, y, min(x+512, image.width), min(y+512, image.height)))
                    index = len(tiles)
                    bounds = (left+x*(right-left)/image.width,
                              top-(y+bitmap.height)*(top-bottom)/image.height,
                              left+(x+bitmap.width)*(right-left)/image.width,
                              top-y*(top-bottom)/image.height)
                    tiles.append(dict(bitmap=bitmap, geometry=bounds, source='@mantis/'+str(key), index=index))
                    parts.append(index)
            decoded[key] = parts
        return decoded[key]

    library_obj = load('sharedassets168.assets').objects[49]
    clips = {clip['name']: clip for clip in library_obj.read_typetree()['clips']}
    expected = {'Lever Hit': (12, [5, 8, 9, 10, 7, 1]),
                'Lever Activated': (30, [1]),
                'Door Open 1': (20, [18, 14, 15, 13]),
                'Door Open 2': (12, [16, 17])}
    for name, (fps, ids) in expected.items():
        clip = clips[name]
        if clip['fps'] != fps or [f['spriteId'] for f in clip['frames']] != ids:
            raise ValueError(f'Unexpected original Mantis clip: {name}')

    def clip_frames(name, start):
        clip = clips[name]
        return [(start + i / clip['fps'],
                 parts_for(resolve(library_obj.assets_file, frame['spriteCollection']), frame['spriteId']))
                for i, frame in enumerate(clip['frames'])]

    owner_of = lambda item: int(item['go'].removesuffix('.json').rsplit('-', 1)[1])
    gate_owners = {owner_of(item) for item in selected if item['name'].startswith('Mantis Gate')}
    for item in selected:
        sprite = read(item['components']['tk2dSprite'][0])['m_Structure']
        initial = parts_for(resolve(scene, sprite['collection']), sprite['_spriteId'])
        owner = owner_of(item)
        if owner in gate_owners:
            trigger = owner
            # Switch .75 s, gate pause .5 s, dust .7 s, then the two clips.
            frames = clip_frames('Door Open 1', 1.95) + clip_frames('Door Open 2', 2.15)
        else:
            fsms = [read(name)['m_Structure']['fsm'] for name in item['components'].get('PlayMakerFSM', [])]
            switch = next(f for f in fsms if f['name'] == 'Switch Control')
            targets = {v['value']['m_PathID'] for v in switch['variables']['gameObjectVariables']
                       if v['value']['m_FileID'] == 0 and v['value']['m_PathID'] in gate_owners}
            if len(targets) != 1:
                raise ValueError('Mantis lever must target exactly one exported gate')
            trigger = targets.pop()
            frames = clip_frames('Lever Hit', 0)
        slots = max(len(initial), *(len(parts) for _, parts in frames))
        if not 1 <= slots <= 4:
            raise ValueError('Mantis sprite exceeds the four-part timeline budget')
        row = next(r for r in terrain_map['inventory'] if r['go'] == item['go'])
        chain = [read(name) for name in row['chain']]
        origin = project((0, 0, 0), chain)
        px = project((sprite['_scale']['x'], 0, 0), chain)
        py = project((0, sprite['_scale']['y'], 0), chain)
        renderer = json.loads((source / item['renderer']).read_text())
        layer = renderer.get('m_SortingLayer', renderer.get('m_SortingLayerID', 0))
        order = renderer['m_SortingOrder']
        color = sprite['_color']
        animations.append(dict(trigger=trigger, owner=owner, slots=slots, alpha=color['a'],
                               initial=initial, frames=frames))
        for slot in range(slots):
            index = initial[min(slot, len(initial)-1)]
            rgba = [color[k] for k in 'rgba']
            if slot >= len(initial):
                rgba[3] = 0
            draws.append(dict(sprite=index, owner=owner, mantis_slot=(owner, slot),
                              values=[px[0]-origin[0], py[0]-origin[0], origin[0],
                                      px[1]-origin[1], py[1]-origin[1], origin[1], origin[2],
                                      *rgba, layer, order,
                                      px[2]-origin[2], py[2]-origin[2], 0],
                              sort=(layer, order, -origin[2], owner, index)))
    return draws, dict(placements=len(selected), renderer_ids=[r['renderer'] for r in selected], source_sha256=hashes,
                       timelines=len(animations), draw_slots=len(draws), _animations=animations)


def write_bank(audit, draws, directories):
    """Resolve sorted draw slots and emit optional shared room animation data."""
    animations = audit.pop('_animations', [])
    binary = bytearray(struct.pack('<4sI', b'MGA1', len(animations)))
    indices = {draw['mantis_slot']: i for i, draw in enumerate(draws) if 'mantis_slot' in draw}
    pad = lambda values: list(values) + [0xffffffff] * (4-len(values))
    for animation in animations:
        binary.extend(struct.pack('<3If', animation['trigger'], animation['slots'],
                                  len(animation['frames']), animation['alpha']))
        slots = [indices[(animation['owner'], slot)] for slot in range(animation['slots'])]
        binary.extend(struct.pack('<4I', *pad(slots)))
        binary.extend(struct.pack('<4I', *pad(animation['initial'])))
        for seconds, sprites in animation['frames']:
            binary.extend(struct.pack('<f4I', seconds, *pad(sprites)))
    for directory in directories:
        path = directory / 'room-visual.bin.switches'
        if animations:
            path.write_bytes(binary)
        else:
            path.unlink(missing_ok=True)
