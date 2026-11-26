"""Compile validated private artwork binding to the native sprite-only plant bank."""
import converted_files  # Install hardlink copy-on-write before asset writes.
import argparse
import hashlib
import json
from pathlib import Path
import struct

from assetripper_client import ROOT
from plant_artwork import validate
from analyze_plants import require
from convert_room_visuals import project


def break_records(source):
    """Read the four anchored Breakable instances and their authored geometry."""
    manifest = json.loads((source / 'plant-effects.json').read_bytes())
    require((manifest['build'], manifest['scene']) == (22529139, 'Tutorial_01'),
            'Unexpected plant effect source')
    room = ROOT / '_private/research/room-visuals'
    room_raw = (room / 'room-visuals-map.json').read_bytes()
    require(hashlib.sha256(room_raw).hexdigest() == manifest['room_manifest_sha256'],
            'Changed room visual manifest')
    room_hashes = json.loads(room_raw)['source_sha256']
    cache = {}

    def read(key):
        require(key == Path(key).name, 'Nonlocal plant dependency')
        if key not in cache:
            hashes = manifest['source_sha256'] if key in manifest['source_sha256'] else room_hashes
            path = (source if key in manifest['source_sha256'] else room) / key
            raw = path.read_bytes()
            require(hashlib.sha256(raw).hexdigest() == hashes[key], 'Changed plant dependency: ' + key)
            cache[key] = json.loads(raw)
        return cache[key]

    def local(ref):
        require(ref['m_FileID'] == 0 and ref['m_PathID'] > 0, 'Nonlocal plant reference')
        return f"level6-{ref['m_PathID']}.json"

    def chain(key):
        transforms = []
        while key:
            transform = read(key)
            transforms.append(transform)
            key = local(transform['m_Father']) if transform['m_Father']['m_PathID'] else None
        return transforms

    records = {}
    for plant in manifest['plants']:
        owner = plant['owner']
        root = read(f'level6-{owner}.json')
        components = {local(c['m_Component']): read(local(c['m_Component']))
                      for c in root['m_Components']}
        transforms = [key for key, value in components.items() if 'm_LocalPosition' in value]
        boxes = [value for value in components.values() if 'm_IsTrigger' in value]
        require(len(transforms) == len(boxes) == 1 and boxes[0]['m_Enabled'] and
                boxes[0]['m_IsTrigger'] and boxes[0]['m_EdgeRadius'] == 0,
                'Unsupported plant collider')
        root_chain = chain(transforms[0])
        box = boxes[0]
        offset, size = box['m_Offset'], box['m_Size']
        corners = [project((offset['m_X'] + x * size['m_X'] / 2,
                            offset['m_Y'] + y * size['m_Y'] / 2, 0), root_chain)
                   for x, y in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
        require(max(p[2] for p in corners)-min(p[2] for p in corners) < 1e-5,
                'Tilted plant collider')
        require(len(plant['remnants']) == 1 and len(plant['debris']) == 2,
                'Unexpected plant parts')
        remnant = int(plant['remnants'][0].split('-')[-1].removesuffix('.json'))
        poses = []
        for debris in plant['debris']:
            require(not manifest['objects'][debris]['active'], 'Active plant debris at start')
            transform = read(manifest['objects'][debris]['components']['Transform'][0])
            poses.extend((transform['m_LocalPosition']['m_Z'],
                          transform['m_LocalEulerAnglesHint']['m_Z']))
        breakable = read(plant['breakable'])['m_Structure']
        require(breakable['wholeRenderer']['m_PathID'] in
                [int(key.split('-')[-1].removesuffix('.json')) for key in components],
                'Plant whole renderer mismatch')
        records[owner] = dict(remnant=remnant, layer=root['m_Layer'],
                              corners=[value for point in corners for value in point[:2]],
                              angle=breakable['angleOffset'], poses=poses)
    require(len(records) == 4, 'Expected four plant break records')
    return records


def convert(binding, room, effects=None):
    validate(binding, room)
    clips = list(binding['clips'])
    require(0 < len(binding['plants']) <= 4 and 0 < len(clips) <= 2, 'Unsupported plant counts')
    _, _, sprites, draws, _ = struct.unpack_from('<4s4I', room)
    if effects is not None:
        require({p['owner'] for p in binding['plants']} == set(effects), 'Plant effect owner mismatch')
    passive = binding.get('passive_plants', [])
    require(len(passive) <= 3 and (not passive or effects is not None), 'Invalid passive plant bank')
    magic = b'HKP3' if passive else b'HKP2' if effects is not None else b'HKP1'
    result = bytearray(struct.pack('<4s5I', magic,
                                   len(binding['plants']), len(clips), sprites, draws, len(passive)))
    for name in clips:
        clip = binding['clips'][name]
        require(len(clip['keys']) <= 64, 'Too many plant keys')
        result.extend(struct.pack('<fI', clip['duration'], len(clip['keys'])))
        for key in clip['keys']:
            result.extend(struct.pack('<fI', key['time'], key['sprite']))
    for plant in binding['plants']:
        base = (plant['owner'], plant['authored_sprite'], clips.index(plant['clip']))
        if effects is None:
            result.extend(struct.pack('<3I', *base))
        else:
            effect = effects[plant['owner']]
            result.extend(struct.pack('<5I13f', *base, effect['remnant'], effect['layer'],
                                      *effect['corners'], effect['angle'], *effect['poses']))
    for plant in passive:
        require(type(plant['random_start']) is bool, 'Invalid passive plant phase policy')
        result.extend(struct.pack('<4I', plant['owner'], plant['authored_sprite'],
                                  clips.index(plant['clip']), int(plant['random_start'])))
    return bytes(result)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / '_private/converted/room-plants-resident')
    parser.add_argument('--output', type=Path, default=ROOT / '_private/converted/room-plants-romfs/plants.bin')
    parser.add_argument('--room', type=Path, default=ROOT / '_private/converted/room-plants-romfs/room-resident-visual.bin')
    parser.add_argument('--effects', type=Path, default=ROOT / '_private/research/plant-effects')
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    room = args.room.read_bytes()
    binding = json.loads((args.source / 'plant-artwork.json').read_bytes())
    raw = convert(binding, room, break_records(args.effects))
    args.output.write_bytes(raw)
    print(json.dumps(dict(plants=len(binding['plants']), clips=len(binding['clips']), bytes=len(raw))))
