"""Export saved CameraLockArea records for generic scene packages."""
import hashlib
import json
import math
import struct
from pathlib import Path

from convert_terrain import paths, transform_point


MAX_LOCKS = 64


def additive_camera_rows(source):
    """Join the saved boss scene's direct CameraLockArea components."""
    source = Path(source)
    path = source / 'false-knight-additive.json'
    if not path.is_file():
        return []
    from convert_room_visuals import additive_owner_id
    additive = json.loads(path.read_text())
    level = additive['source_scene']
    root = source / 'visuals' / level
    objects = additive['scene_objects']
    transforms = {c['id']: o for o in objects for c in o['components'] if c['type'] == 'Transform'}
    rows = []
    for obj in objects:
        if not obj['name'].startswith('CameraLockArea'):
            continue
        direct = [f'{level}-{c["id"]}.json' for c in obj['components'] if c['type'] == 'MonoBehaviour'
                  and 'cameraXMin' in json.loads((root / f'{level}-{c["id"]}.json').read_text()).get('m_Structure', {})]
        if not direct:
            continue
        chain, owners, visited = [], [], set()
        parent = obj
        while True:
            if parent['id'] in visited:
                raise ValueError('Cyclic additive camera hierarchy')
            visited.add(parent['id'])
            owners.append(f'{level}-{parent["id"]}.json')
            chain.extend(f'{level}-{c["id"]}.json' for c in parent['components'] if c['type'] == 'Transform')
            if not parent['parent']:
                break
            parent = transforms[parent['parent']]
        for collider in obj['components']:
            if collider['type'] != 'BoxCollider2D':
                continue
            rows.append((dict(go=f'{level}-{obj["id"]}.json', owners=owners, chain=chain,
                              collider=f'{level}-{collider["id"]}.json', type='BoxCollider2D',
                              components=[{'CameraLockArea': direct}]), root, additive_owner_id(obj['id'])))
    return rows


def authored_camera_limits(source, scene):
    """Return verified TileMap center limits, or None for older saved exports."""
    root = Path(source) / 'camera'
    path = root / 'camera-map.json'
    if not path.is_file():
        return None
    manifest = json.loads(path.read_text())
    if manifest.get('build') != 22529139 or manifest.get('scene') != scene:
        raise ValueError('Camera source build/scene mismatch')
    cache = {}
    for name, expected in manifest['source_sha256'].items():
        if Path(name).name != name or not name.endswith('.json'):
            raise ValueError('Invalid camera source filename')
        raw = (root / name).read_bytes()
        if hashlib.sha256(raw).hexdigest() != expected:
            raise ValueError('Changed camera source: ' + name)
        cache[name] = json.loads(raw)
    active = []
    for row in manifest['tilemaps']:
        if not row['owners'] or row['owners'][0] != row['go']:
            raise ValueError('Camera source requires the complete owner chain')
        try:
            component = cache[row['component']]
            owners = [cache[name] for name in row['owners']]
        except KeyError as error:
            raise ValueError('Unhashed camera source reference') from error
        # GameManager.GetTileMap reads the tagged object's component without
        # filtering enabled behaviours (Waterways_15's map is disabled); the
        # dimensions remain scene metadata when its behaviour is disabled.
        if all(owner.get('m_IsActive') for owner in owners):
            data = component['m_Structure']
            width, height = data['width'], data['height']
            if any(type(v) is not int or not 0 < v <= 100000 for v in (width, height)):
                raise ValueError('Invalid source TileMap dimensions')
            active.append((width, height))
    if len(active) != 1:
        raise ValueError('Camera source requires exactly one active TileMap')
    width, height = active[0]
    # CameraController.GetTilemapInfo/KeepWithinSceneBounds, build 22529139:
    # min is clamped before max, so an inverted interval resolves to its max.
    return [min(14.6, width - 14.6), min(8.3, height - 8.3),
            width - 14.6, height - 8.3]


def export_camera_locks(source, target, scene_limits=None):
    """Write ``scene-camera.bin`` from direct, active CameraLockArea owners.

    ``source`` is the saved scene root containing ``terrain/terrain-map.json``;
    ``target`` is the package directory. A lock is emitted once per direct local
    component, using its enabled collider row as the trigger.
    """
    root = Path(source) / 'terrain'
    manifest = json.loads((root / 'terrain-map.json').read_text())
    records = []
    lock_owners = []
    seen = set()
    inventory = [(row, root, None) for row in manifest['inventory']]
    inventory.extend(additive_camera_rows(source))
    for row, component_root, additive_owner in inventory:
        read = lambda name: json.loads((component_root / name).read_text())
        components = row.get('components', [])
        owners = row.get('owners', [])
        if not components or not owners or not isinstance(components[0], dict):
            continue
        direct = components[0].get('CameraLockArea', [])
        if not direct:
            continue
        collider = read(row['collider'])
        if not collider.get('m_Enabled') or not all(read(owner).get('m_IsActive') for owner in owners):
            continue
        for component_name in direct:
            if component_name in seen:
                continue
            component = read(component_name)
            if not component.get('m_Enabled'):
                continue
            data = component.get('m_Structure', {})
            required = ('cameraXMin', 'cameraYMin', 'cameraXMax', 'cameraYMax',
                        'preventLookUp', 'preventLookDown', 'maxPriority')
            if any(key not in data for key in required):
                raise ValueError(f'{component_name}: incomplete CameraLockArea data')
            chain = [read(name) for name in row['chain']]
            points = [transform_point(point, chain)
                      for path in paths(collider, row['type']) for point in path]
            if not points:
                raise ValueError(f'{row["collider"]}: CameraLockArea collider has no points')
            trigger = [min(p[0] for p in points), min(p[1] for p in points),
                       max(p[0] for p in points), max(p[1] for p in points)]
            limits = [data['cameraXMin'], data['cameraYMin'],
                      data['cameraXMax'], data['cameraYMax']]
            if all(value == 0 for value in limits):
                continue  # CameraLockArea.ValidateBounds rejects this disabled lock.
            if any(value < 0 for value in limits):
                if scene_limits is None:
                    raise ValueError('Camera lock sentinel requires room camera limits')
                # approximation: the source uses TileMap camera bounds; saved rooms
                # currently fit those defaults to converted static room extents.
                limits = [scene_limits[i] if value < 0 else value for i, value in enumerate(limits)]
            # CameraController.KeepWithinLockBounds clamps to min, then max.
            # An authored inverted interval therefore resolves to its upper end.
            for lower, upper in ((0, 2), (1, 3)):
                limits[lower] = min(limits[lower], limits[upper])
            values = trigger + limits
            if (not all(math.isfinite(float(v)) and abs(float(v)) <= 100000 for v in values)
                    or trigger[0] >= trigger[2] or trigger[1] >= trigger[3]
                    or limits[0] > limits[2] or limits[1] > limits[3]):
                raise ValueError(f'{component_name}: invalid camera lock rectangle')
            flags = ((1 if data['preventLookUp'] else 0)
                     | (2 if data['preventLookDown'] else 0)
                     | (4 if data['maxPriority'] else 0))
            records.append((*map(float, trigger), *map(float, limits), flags))
            suffix = Path(row['go']).stem.rsplit('-', 1)[-1]
            lock_owners.append(additive_owner if additive_owner is not None else int(suffix) if suffix.isdecimal() else 0)
            seen.add(component_name)
    if len(records) > MAX_LOCKS:
        raise ValueError(f'too many camera locks: {len(records)} > {MAX_LOCKS}')
    with_owners = any(lock_owners)
    output = bytearray(struct.pack('<4sI', b'HCL2' if with_owners else b'HCL1', len(records)))
    for record, owner in zip(records, lock_owners):
        output.extend(struct.pack('<8fI', *record))
        if with_owners:
            output.extend(struct.pack('<I', owner))
    target = Path(target)
    target.mkdir(parents=True, exist_ok=True)
    (target / 'scene-camera.bin').write_bytes(output)
    return records
