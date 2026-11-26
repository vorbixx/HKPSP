"""Convert every initially active Tutorial_01 Terrain contour, preserving fixtures.

Tilemap dimensions are metadata, never clipping bounds. The selection window
encloses every exported collider; inactive objects stay in the audit.
"""
import converted_files  # Install hardlink copy-on-write before asset writes.
import argparse
import hashlib
import json
import shutil
import struct
from pathlib import Path
from assetripper_client import ROOT
from private_output_paths import is_private_output
from convert_terrain import MAX_SHAPES, convert, require, transform_point


# Static prop colliders are part of the room snapshot even though they are not
# on the Terrain layer.  Keep this inventory source-backed and validate every
# record below before it can enter the runtime HKT1 bank.
STATIC_SOLID_PROPS = (
    ('level6-8196.json', 'level6-1566.json', 'level6-5438.json'),
    ('level6-8205.json', 'level6-1632.json', 'level6-5502.json'),
    ('level6-8253.json', 'level6-2357.json', 'level6-6219.json'),
    ('level6-8288.json', 'level6-2800.json', 'level6-6657.json'),
    ('level6-8320.json', 'level6-3422.json', 'level6-7288.json'),
)


def append_static_solids(report, source, mapping, read):
    physics = read(mapping['physics'])
    for collider_name, go_name, transform_name in STATIC_SOLID_PROPS:
        collider, go = read(collider_name), read(go_name)
        if not collider.get('m_Enabled') or collider.get('m_IsTrigger'):
            raise ValueError('Static prop collider is not enabled/solid: ' + collider_name)
        if go.get('m_Layer') != 19 or not go.get('m_Name', '').startswith('Geo Rock'):
            raise ValueError('Unexpected static prop identity: ' + go_name)
        # HeroController.CheckForTerrainThunk uses layers 8 and 25, regardless
        # of the actor collision matrix. Keep the query geometry for other casts.
        flags = actor_contact_flags(physics, go['m_Layer'])
        if not ((1 << go['m_Layer']) & 33554688):
            flags |= 64
        # WalkLeftRight probes only Terrain, independently of actor/nail masks.
        if go['m_Layer'] != 8:
            flags |= 128
        size, offset = collider['m_Size'], collider['m_Offset']
        transform = read(transform_name)
        require(transform['m_Father']['m_PathID'] == 6759, 'Unexpected static prop parent')
        points = [[-size['m_X']/2 + offset['m_X'], -size['m_Y']/2 + offset['m_Y']],
                  [ size['m_X']/2 + offset['m_X'], -size['m_Y']/2 + offset['m_Y']],
                  [ size['m_X']/2 + offset['m_X'],  size['m_Y']/2 + offset['m_Y']],
                  [-size['m_X']/2 + offset['m_X'],  size['m_Y']/2 + offset['m_Y']]]
        parent_chain = [transform, read('level6-6759.json')]
        world = transform_point([0, 0], parent_chain)
        world_points = [transform_point(p, parent_chain) for p in points]
        shape = dict(source=collider_name, id=int(collider_name.split('-')[-1].split('.')[0]),
                     name=go['m_Name'], kind=0, flags=flags, points=world_points,
                     material=report['actor_material'], material_source=mapping['default_material'],
                     components=['StaticSolidProp'])
        report['shapes'].append(shape)
        report.setdefault('static_solids', []).append(dict(source=collider_name, name=go['m_Name'],
                                                            position=world, included=True))


def actor_contact_flags(physics, layer):
    # HKT1 shares contacts between Hero(9), enemies(11), Geo(12), corpse(26).
    # A new source layer must agree for all four or needs per-body filtering.
    contacts = [bool(physics['m_LayerCollisionMatrix'][body] & (1 << layer))
                for body in (9, 11, 12, 26)]
    require(len(set(contacts)) == 1, 'Mixed actor collision layers need per-body filtering')
    return 0 if contacts[0] else 32


def write_hkt1(report, path):
    shapes, n = report['shapes'], sum(len(s['points']) for s in report['shapes'])
    if not shapes or len(shapes) > MAX_SHAPES or n > 8192:
        raise ValueError('Static room geometry exceeds HKT1 capacity')
    data = bytearray(struct.pack('<4s4I10f', b'HKT1', len(shapes), n, 1, 0,
                                 *report['crawler_spawn'], *report['region'],
                                 *report['actor_material'], *report['corpse_material']))
    first = 0
    for shape in shapes:
        data.extend(struct.pack('<5I2f', shape['id'], shape['kind'], shape['flags'],
                                first, len(shape['points']), *shape['material']))
        first += len(shape['points'])
    for shape in shapes:
        for point in shape['points']:
            data.extend(struct.pack('<2f', *point))
    path.write_bytes(data)
    return data


def build(source, output, romfs):
    for path in (output, romfs):
        if not converted_files.private_output(path, ROOT, allow_build=True):
            raise ValueError('Output must remain private')
        path.mkdir(parents=True, exist_ok=True)
    # Existing converter retains contours whole and verifies every source hash.
    # Its wide selection is intentionally distinct from tilemap dimensions.
    convert(source, output / 'geometry', output / 'staging', [-1000, -1000, 1000, 1000])
    report = json.loads((output / 'geometry/manifest.json').read_text())
    mapping = json.loads((source / 'terrain-map.json').read_text())
    cache = {}
    def read(name):
        if name not in cache:
            cache[name] = json.loads((source / name).read_text())
        return cache[name]
    if mapping['scene']=='Tutorial_01':
        append_static_solids(report, source, mapping, read)
    audit = []
    for row in mapping['inventory']:
        collider, go = read(row['collider']), read(row['go'])
        enabled = bool(collider['m_Enabled']) and all(read(k)['m_IsActive'] for k in row['owners'])
        components = sorted({k for group in row['components'] for k in group})
        try:position=transform_point([0, 0], [read(k) for k in row['chain']])
        except ValueError:
            if mapping['scene']=='Tutorial_01':raise
            position=None # Audited excluded 3D prop, never used as terrain.
        audit.append(dict(source=row['collider'], name=go['m_Name'], layer=go['m_Layer'],
                          active=enabled, trigger=bool(collider['m_IsTrigger']), type=row['type'],
                          position=position,
                          components=components, owners=row['owners'],
                          included=any(s['source']==row['collider'] for s in report['shapes'])))
    shutil.copyfile(output/'geometry/terrain-motion.bin',romfs/'terrain-motion.bin')
    shutil.copyfile(output/'geometry/terrain-walls.bin',romfs/'terrain-walls.bin')
    shutil.copyfile(output/'geometry/terrain-levers.bin',romfs/'terrain-levers.bin')
    binary = write_hkt1(report, output / 'geometry/tutorial.bin')
    report['binary_sha256'] = hashlib.sha256(binary).hexdigest()
    for path in (output / 'tutorial-room.bin', romfs / 'tutorial-room.bin', romfs / 'tutorial.bin'):
        path.write_bytes(binary)
    report.update(selection_is_room_bounds=False, binary='tutorial-room.bin', collider_audit=audit,
                  limitations=['Initial serialized active state; room FSM may change objects after Awake/Start.',
                  'Non-Terrain colliders outside the source-backed static-solid inventory are audited, not converted.',
                               'Dynamic gameplay and hazards require explicit runtime implementations.'])
    (output / 'manifest.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(dict(shapes=len(report['shapes']), vertices=sum(len(s['points']) for s in report['shapes']),
                          colliders=len(audit), sha256=hashlib.sha256(binary).hexdigest())))


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, default=ROOT / '_private/research/terrain')
    p.add_argument('--output', type=Path, default=ROOT / '_private/converted/room-terrain')
    p.add_argument('--romfs', type=Path, default=ROOT / '_private/converted/hero/romfs')
    a = p.parse_args()
    build(a.source, a.output, a.romfs)
