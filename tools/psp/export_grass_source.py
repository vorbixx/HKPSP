"""Export the King's Pass grass sources read by `tools/grass_artwork.py`.

Writes `_private/research/grass-artwork-source`:
- `inventory.json`: the 41 Tutorial_01 GrassCut owners with their trigger
  collider, whole/stub renderers and cut parameters (AssetRipper JSON);
- `clip-summary.json`: the five GrassNPush sprite clips;
- `decoded-selected.json`: the grass Sprites and their SpriteAtlas (UnityPy);
- `prior/raw-selected-objects.json`: the serialized particle child objects.
The individual AssetRipper records are kept under `assets/`. Load Tutorial_01
(level6) into AssetRipper first.
"""
import argparse
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/psp'))
sys.path.insert(0, str(ROOT / 'tools'))
from assetripper_client import Client
from export_steam_base import collection_path
from extract_crawler import Export

BUILD = 22529139
SCENE = 'level6'
STUB_SPRITES = (1931, 2258, 2353, 3558, 3623)  # grass_artwork.load requires these replacement frames
PUSH_CLIPS = tuple(f'Grass{index}Push' for index in range(1, 6))


def encode(value):
    """UnityPy byte fields as hex, matching the saved research layout."""
    if isinstance(value, (bytes, bytearray)):
        return {'hex': bytes(value).hex()}
    raise TypeError(type(value).__name__)


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, default=encode), encoding='utf-8')


def unity_files(game_data):
    import UnityPy
    sys.path.insert(0, str(ROOT / 'tools'))
    from watcher_unity import AlignedGenerator
    env = UnityPy.Environment()
    generator = AlignedGenerator('6000.0.61f1')
    generator.load_local_dll_folder(str(game_data / 'Managed'))
    env.typetree_generator = generator
    files = {}
    for name in (SCENE, 'resources.assets', 'globalgamemanagers.assets', 'sharedassets6.assets'):
        env.load_file(str(game_data / name))
        files[name] = next(f for f in env.files.values() if getattr(f, 'name', None) == name)
    return files


def external(files, owner, ref):
    name = owner.name if ref['m_FileID'] == 0 else Path(owner.externals[ref['m_FileID'] - 1].path).name
    return files[name], ref['m_PathID']


def grass_owners(files):
    """GameObjects in level6 carrying a GrassCut MonoBehaviour, with its fields."""
    scene = files[SCENE]
    owners = {}
    for obj in scene.objects.values():
        if obj.type.name != 'MonoBehaviour':
            continue
        header = obj.read(check_read=False)
        script_file, script_id = external(files, scene, {'m_FileID': header.m_Script.m_FileID,
                                                          'm_PathID': header.m_Script.m_PathID})
        if script_file.objects[script_id].read().m_ClassName != 'GrassCut':
            continue
        owners[header.m_GameObject.m_PathID] = obj.read_typetree()
    return owners


def world_position(export, collection, transform):
    """Compose the 2D parent chain: rotation about Z and scale, as Unity does."""
    import math
    x = y = 0.0
    z = 0.0
    node = transform
    chain = []
    while True:
        chain.append(node)
        father = node['m_Father']
        if not father['m_PathID']:
            break
        node, _, _ = export.resolve(collection, father)
    for node in chain:
        p, s, q = node['m_LocalPosition'], node['m_LocalScale'], node['m_LocalRotation']
        angle = 2 * math.atan2(q['m_Z'], q['m_W'])
        c, sn = math.cos(angle), math.sin(angle)
        x, y = c * x * s['m_X'] - sn * y * s['m_Y'] + p['m_X'], sn * x * s['m_X'] + c * y * s['m_Y'] + p['m_Y']
        z = z * s['m_Z'] + p['m_Z']
    return [x, y, z]


def export(client, game_data, output):
    files = unity_files(game_data)
    owners = grass_owners(files)
    export = Export(client, output / 'assets')
    collection = collection_path(client, SCENE)
    inventory, sprite_ids, particle_objects = [], set(STUB_SPRITES), set()
    # The HKG1 record order follows the whole-grass renderer IDs (GrassCut.disable).
    for owner in sorted(owners, key=lambda o: owners[o]['disable'][0]['m_PathID']):
        parts = export.components(collection, owner)
        cut = export.cache[parts['GrassCut'][0]]['m_Structure']
        transform = export.cache[parts['Transform'][0]]
        colliders = [dict(data=export.cache[key]) for kind, keys in parts.items()
                     if kind.endswith('Collider2D') for key in keys]
        renderers = []
        for role, field in (('whole', 'disable'), ('stub', 'enable')):
            for ref in cut[field]:
                renderer, _, target = export.resolve(collection, ref)
                game_object = renderer['m_GameObject']['m_PathID']
                sprite, sprite_key, _ = export.resolve(target, renderer['m_Sprite'])
                sprite_ids.add(renderer['m_Sprite']['m_PathID'])
                local = export.cache[export.components(collection, game_object)['Transform'][0]]
                renderers.append(dict(role=role, owner=f'{SCENE}-{game_object}.json',
                                      sprite_name=sprite['m_Name'], sprite_key=sprite_key,
                                      sprite_ref=renderer['m_Sprite'], local_transform=local))
        particle_objects.add(cut['particles']['m_PathID'])
        inventory.append(dict(owner=owner, cut_parameters={k: cut[k] for k in (
            'disable', 'enable', 'disableColliders', 'enableColliders', 'particles', 'cutEffectPrefab')},
            world_position=world_position(export, collection, transform),
            local_scale=transform['m_LocalScale'], colliders=colliders, renderers=renderers))
    write(output / 'inventory.json', dict(build=BUILD, scene='Tutorial_01', owners=inventory))

    clips = []
    for row in client.search('Push'):
        if row['type'] != 'AnimationClip' or row['name'] not in PUSH_CLIPS:
            continue
        clip, _ = export.fetch(row['path']['C'], row['path']['D'])
        curve = next(c for c in clip['m_PPtrCurves'] if c['m_Attribute'] == 'm_Sprite')['m_Curve']
        frames = []
        for key in curve:
            _, sprite_key, _ = export.resolve(row['path']['C'], key['m_Value'])
            name, ident = sprite_key.removesuffix('.json').rsplit('-', 1)
            sprite_ids.add(int(ident))
            frames.append(dict(time=key['m_Time'], sprite=f'{name}:{ident}'))
        clips.append(dict(name=row['name'], stop=clip['m_MuscleClip']['m_StopTime'], frames=frames))
    if sorted(c['name'] for c in clips) != list(PUSH_CLIPS):
        raise ValueError(f'Expected the five grass Push clips, found {[c["name"] for c in clips]}')
    clips.sort(key=lambda c: c['name'])
    write(output / 'clip-summary.json', dict(clips=clips))

    resources = files['resources.assets']
    decoded = {}
    for ident in sorted(sprite_ids):
        sprite = resources.objects[ident].read_typetree()
        decoded[f'resources.assets:{ident}'] = dict(type='Sprite', data=sprite)
        atlas = sprite['m_SpriteAtlas']
        if atlas['m_PathID']:
            atlas_file, atlas_id = external(files, resources, atlas)
            key = f'{atlas_file.name}:{atlas_id}'
            if key not in decoded:
                decoded[key] = dict(type='SpriteAtlas', data=atlas_file.objects[atlas_id].read_typetree())
    write(output / 'decoded-selected.json', dict(objects=decoded))

    scene = files[SCENE]
    raw = {}

    def record(ident):
        obj = scene.objects[ident]
        data = obj.get_raw_data()
        raw[str(ident)] = dict(class_id=int(obj.type), size=len(data), hex=data.hex())
        return obj

    for ident in sorted(particle_objects):
        game_object = record(ident)
        components = [entry['component']['m_PathID'] for entry in game_object.read_typetree()['m_Component']]
        raw[str(ident)]['component'] = dict(components=[dict(path_id=c) for c in components])
        for component in components:
            record(component)
    write(output / 'prior/raw-selected-objects.json', dict(queries=[dict(label=SCENE, objects=raw)]))
    print(f'Grass: {len(inventory)} owners, {len(clips)} push clips, {len(decoded)} sprite records, '
          f'{len(raw)} raw objects')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--port', type=int, default=58291)
    parser.add_argument('--game-data', type=Path, default=ROOT / '_private/original/Hollow Knight/hollow_knight_Data')
    parser.add_argument('--output', type=Path, default=ROOT / '_private/research/grass-artwork-source')
    args = parser.parse_args()
    export(Client(args.port), args.game_data, args.output)


if __name__ == '__main__':
    main()
