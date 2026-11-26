"""Export the tk2d tilemap source and its complete GameObject owner chain."""
import argparse
import json
import os
import tempfile
from pathlib import Path

from assetripper_client import Client, ROOT
from extract_crawler import Export

BUILD = 22529139
TILEMAP_TYPE = 'tk2dTileMap'


def owner_chain(export, collection, game_object_id):
    parts = export.components(collection, game_object_id)
    transforms = parts.get('Transform', [])
    if len(transforms) != 1:
        raise ValueError('Tilemap owner must have exactly one Transform')
    owners, visited = [], set()
    transform_key = transforms[0]
    while True:
        if transform_key in visited:
            raise ValueError('Cyclic tilemap owner hierarchy')
        visited.add(transform_key)
        transform = export.cache[transform_key]
        _, owner_key, _ = export.resolve(collection, transform['m_GameObject'])
        owners.append(owner_key)
        father = transform['m_Father']
        if not father['m_PathID']:
            return owners
        _, transform_key, collection = export.resolve(collection, father)


def write_manifest(path, manifest):
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix='.' + path.name + '.', dir=path.parent)
    try:
        with os.fdopen(descriptor, 'w', encoding='utf-8', newline='\n') as stream:
            json.dump(manifest, stream, indent=2, ensure_ascii=False)
            stream.write('\n')
        os.replace(temporary, path)
    except BaseException:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        raise


def _append_tilemap(export, collection, tilemap, component_key, tilemaps, seen):
    if component_key in seen:
        return
    go_ref = tilemap['m_GameObject']
    _, go_key, go_collection = export.resolve(collection, go_ref)
    owners = owner_chain(export, go_collection, go_ref['m_PathID'])
    tilemaps.append(dict(component=component_key, go=go_key, owners=owners))
    seen.add(component_key)


def extract(client, output, collection, scene):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    export, tilemaps, seen = Export(client, output), [], set()

    # Search by the class name first, rejecting MonoScript/owner GameObject rows.
    for row in client.search(TILEMAP_TYPE):
        if row.get('collection') != collection:
            continue
        row_type = row.get('type')
        if row_type not in (TILEMAP_TYPE, 'MonoBehaviour'):
            continue
        path = row.get('path', {})
        asset_collection, asset_id = path.get('C'), path.get('D')
        if asset_collection is None or asset_id is None:
            raise ValueError('Tilemap search row has no asset path')
        tilemap, component_key = export.fetch(asset_collection, asset_id)
        if row_type == 'MonoBehaviour':
            script, _, _ = export.resolve(asset_collection, tilemap['m_Script'])
            if script.get('m_ClassName') != TILEMAP_TYPE:
                continue
        _append_tilemap(export, asset_collection, tilemap, component_key, tilemaps, seen)

    # AssetRipper name search may expose TileMap GameObjects, while the actual
    # tk2dTileMap MonoBehaviour is discoverable only through their components.
    for row in client.search('TileMap'):
        if row.get('collection') != collection or row.get('type') != 'GameObject':
            continue
        path = row.get('path', {})
        asset_collection, asset_id = path.get('C'), path.get('D')
        if asset_collection is None or asset_id is None:
            raise ValueError('TileMap GameObject search row has no asset path')
        parts = export.components(asset_collection, asset_id)
        for component_key in parts.get(TILEMAP_TYPE, []):
            tilemap = export.cache[component_key]
            _append_tilemap(export, asset_collection, tilemap, component_key,
                            tilemaps, seen)

    if not tilemaps:
        raise ValueError(f'{scene}: no actual tk2dTileMap components found in {collection}')
    manifest = dict(build=BUILD, scene=scene,
                    tilemaps=sorted(tilemaps, key=lambda row: row['component']),
                    source_sha256=export.hashes)
    write_manifest(output / 'camera-map.json', manifest)
    print(json.dumps(dict(scene=scene, tilemaps=len(tilemaps), files=len(export.hashes))))
    return manifest


def private_scene_output(scene):
    from eastern_rooms import scene_source
    from extract_scenes import SCENES
    if scene not in SCENES:
        raise ValueError(f'{scene}: not a registered scene')
    output = (ROOT / scene_source(scene) / 'camera').resolve()
    private_roots = ((ROOT / '_private/research').resolve(),
                     (ROOT / '_private/minimum-map-east').resolve())
    if not any(output.is_relative_to(root) for root in private_roots):
        raise ValueError('Camera exports must remain private')
    return output, SCENES[scene]


def main(argv=None):
    from extract_scenes import SCENES
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scene', required=True, choices=sorted(SCENES))
    parser.add_argument('--port', type=int, default=58291)
    args = parser.parse_args(argv)
    output, index = private_scene_output(args.scene)
    extract(Client(args.port), output, f'level{index}', args.scene)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
