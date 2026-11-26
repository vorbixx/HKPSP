"""Export the serialized source records for the King's Pass lighting bank."""
import json

from extract_crawler import Export, write_asset
from inspect_fsm import decode_fsm
from restore_managed_research import verify_collection
from scene_lighting import SCENE_LIGHT, scene_ambient


def collection(client, query, name):
    paths = {json.dumps(row['path']['C'], sort_keys=True)
             for row in client.search(query) if row['collection'] == name}
    if len(paths) != 1:
        raise ValueError(f'Expected one AssetRipper collection named {name}')
    path = json.loads(paths.pop())
    verify_collection(client, path, name)
    return path


def export_sprite(export, resources, identity):
    sprite, _ = export.fetch(resources, identity)
    texture = sprite['m_RD']['m_Texture']
    _, filename, target = export.resolve(resources, texture)
    image = export.client.request('/Assets/Image',
        Path=json.dumps({'C': target, 'D': texture['m_PathID']}), Extension='png')
    if not image.startswith(b'\x89PNG\r\n\x1a\n'):
        raise ValueError(f'Expected the original texture PNG for sprite {identity}')
    write_asset(export.output / (filename.removesuffix('.json') + '.png'), image)


def export_fsm(export, path, identity, filename):
    raw, _ = export.fetch(path, identity)
    fsm = decode_fsm(raw['m_Structure']['fsm'])
    write_asset(export.output / filename, (json.dumps(fsm, indent=2) + '\n').encode('utf-8'))


def extract(client, output):
    resources = collection(client, 'white_light_donut', 'resources.assets')
    level = collection(client, 'SceneManager', 'level6')
    export = Export(client, output)
    for identity in (487, 2862):
        export.fetch(resources, identity)
    for identity in (3023, 2642):
        export_sprite(export, resources, identity)
    for identity in (20931, 23559):
        export_fsm(export, resources, identity, f'fsm-{identity}.json')
    for identity in (12051, 12170):
        export_fsm(export, level, identity, f'decoded-level6-{identity}.json')
    for identity in (12820, 4384, 6029, 8134, 8135, 8136, 8235, 8237, 8238):
        export.fetch(level, identity)
    scene_ambient(output / SCENE_LIGHT.name)
    write_asset(output / 'lighting-export.json', b'{"build":22529139}\n')


def extract_donut(client, output):
    resources = collection(client, 'white_light_donut', 'resources.assets')
    export = Export(client, output)
    for identity in (512, 81):
        export.fetch(resources, identity)
    export_sprite(export, resources, 3318)
    write_asset(output / 'lighting-export.json', b'{"build":22529139}\n')
