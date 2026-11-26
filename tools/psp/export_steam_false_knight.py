"""Export the False Knight arena's authored additive scene."""
import hashlib
import json
from pathlib import Path
import tempfile

from extract_crawler import Export
from extract_room_visuals import extract


LEVEL = 'level48'
SNAPSHOT = 'false-knight-additive.json'


def snapshot(level):
    import UnityPy

    expected = 'a5e41672c76cae7f4be9fbf0b940643e241849d7c2533bd0d6f88e56f8db46d4'
    if hashlib.sha256(level.read_bytes()).hexdigest() != expected:
        raise ValueError('False Knight additive export requires the reviewed Steam scene')
    scene = UnityPy.load(str(level)).file
    objects = []
    for owner in scene.objects.values():
        if owner.type.name != 'GameObject':
            continue
        obj = owner.read()
        transform = obj.m_Component[0].component.deref()
        if transform.type.name != 'Transform':
            raise ValueError('Expected a Transform on the False Knight additive object')
        transform = transform.read_typetree()
        components = []
        for entry in obj.m_Component:
            pointer = entry.component
            value = dict(id=pointer.path_id, type=pointer.type.name)
            if pointer.type.name in ('BoxCollider2D', 'PolygonCollider2D'):
                value['geometry'] = pointer.deref().read_typetree()
            components.append(value)
        objects.append(dict(id=owner.path_id, name=obj.m_Name, active=obj.m_IsActive,
                            parent=transform['m_Father']['m_PathID'], local_transform=transform,
                            components=components))
    return dict(build=22529139, source_scene=LEVEL, source_sha256=expected, scene_objects=objects)


def export_cameras(client, visuals, data):
    result = Export(client, visuals)
    cameras = [r for r in client.search('CameraLockArea') if r['collection'] == LEVEL
               and r['type'] == 'GameObject']
    if len(cameras) != 2:
        raise ValueError('Expected two False Knight additive camera locks')
    collection = cameras[0]['path']['C']
    transforms = {obj['components'][0]['id']: obj for obj in data['scene_objects']}
    for row in cameras:
        parts = result.components(collection, row['path']['D'])
        transform = result.cache[parts['Transform'][0]]
        while transform['m_Father']['m_PathID']:
            owner = transforms[transform['m_Father']['m_PathID']]
            parts = result.components(collection, owner['id'])
            transform = result.cache[parts['Transform'][0]]
    path = visuals / 'room-visuals-map.json'
    manifest = json.loads(path.read_text(encoding='utf-8'))
    manifest['source_sha256'].update(result.hashes)
    path.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')


def prepare(client, game_data, room):
    destination = room / 'visuals' / LEVEL
    metadata = room / SNAPSHOT
    if destination.resolve() != destination or metadata.resolve() != metadata:
        raise ValueError(f'False Knight export requires direct destinations in {room}')
    data = snapshot(game_data / LEVEL)
    with tempfile.TemporaryDirectory(prefix='.false-knight-', dir=room) as temporary:
        pending = Path(temporary)
        if not destination.exists():
            visuals = pending / LEVEL
            extract(client, visuals, LEVEL, 'Crossroads_10')
            export_cameras(client, visuals, data)
            visuals.rename(destination)
        elif not (destination / 'room-visuals-map.json').is_file():
            raise ValueError(f'Incomplete False Knight visual export: {destination}')
        path = pending / SNAPSHOT
        path.write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8')
        path.replace(metadata)
