"""Export inventory-selected gameplay scenes from an isolated AssetRipper session.

Run each --region batch through validation-lock.ps1 -Exclusive. This only saves
source exports; the legacy SCENES below remains the converter's packaging list.
"""
import argparse
import hashlib
import json
import os
import traceback
from concurrent.futures import ThreadPoolExecutor
from contextlib import nullcontext
from functools import cache, lru_cache
from pathlib import Path
from assetripper_client import Client, ROOT
from extract_crawler import Export
from extract_terrain import extract as terrain
from door_transitions import prompt_binding, prompt_marker
from extract_room_visuals import extract as visuals
from extract_room_visuals import has_breakable_export, supplement_breakables
from extract_scene_camera import extract as scene_camera
from extract_scene_borders import extract_prefab as scene_borders
from extract_scene_borders import valid_export as valid_scene_borders
from scene_camera import authored_camera_limits
from extract_room_tilemap import extract_tilemap
from fungal_gardens_scenes import SCENES as FUNGAL_GARDENS_SCENES
from mines_waterways_scenes import SCENES as MINES_WATERWAYS_SCENES
from deepnest_scenes import SCENES as DEEPNEST_SCENES
from eastern_rooms import ADMITTED, B2_CANDIDATES, scene_source

# Converter compatibility: exporting a room must not opt it into the runtime.
SCENES = {
    'Crossroads_01': 37,
    'Crossroads_02': 38,
    'Crossroads_03': 39,
    'Crossroads_04': 40,
    'Crossroads_05': 41,
    'Crossroads_07': 43,
    'Crossroads_10': 46,
    'Crossroads_11_alt': 50,
    'Crossroads_14': 53,
    'Crossroads_16': 55,
    'Crossroads_19': 57,
    'Crossroads_25': 60,
    'Crossroads_33': 64,
    'Crossroads_38': 68,
    'Crossroads_39': 69,
    'Crossroads_50': 81,
    'RestingGrounds_02': 237,
    'RestingGrounds_04': 239,
    'RestingGrounds_05': 240,
    'RestingGrounds_06': 241,
    'Town': 7,
    'Crossroads_06': 42,
    'Crossroads_ShamanTemple': 76,
    'Room_ruinhouse': 20,
    'Fungus1_01': 128,
    'Fungus3_archive_02': 228,
    'Fungus1_01b': 129,
    'Fungus1_02': 130,
    'Fungus1_17': 146,
    'Fungus1_03': 131,
    'Fungus1_31': 158,
    'Fungus1_32': 159,
    'Fungus1_21': 149,
    'Fungus1_04': 132,
    'Fungus1_06': 135,
    'Crossroads_08': 44,
    'Crossroads_09': 45,
    'Crossroads_10': 46,
    'Crossroads_12': 51,
    'Crossroads_13': 52,
    'Crossroads_15': 54,
    'Crossroads_18': 56,
    'Crossroads_21': 58,
    'Crossroads_22': 59,
    'Crossroads_27': 61,
    'Crossroads_30': 62,
    'Crossroads_31': 63,
    'Crossroads_35': 65,
    'Crossroads_36': 66,
    'Crossroads_37': 67,
    'Crossroads_40': 70,
    'Crossroads_42': 71,
    'Crossroads_43': 72,
    'Crossroads_45': 73,
    'Crossroads_46': 74,
    'Crossroads_47': 77,
    'Crossroads_48': 78,
    'Crossroads_49': 79,
    'Crossroads_52': 82,
    'Room_temple': 19,
    'Room_ruinhouse': 20,
    'Room_Charm_Shop': 10,
    'Room_Mender_House': 11,
    'Room_Tram_RG': 24,
    'Room_Ouiji': 28,
    'Room_mapper': 12,
    'Room_Town_Stag_Station': 9,
    'Room_shop': 17,
    'Room_Bretta': 25,
    'Grimm_Divine': 390,
    'Grimm_Main_Tent': 391,
    'Room_Sly_Storeroom': 18,
    'Room_Bretta_Basement': 26,
}
SCENES.update(FUNGAL_GARDENS_SCENES)
SCENES.update(MINES_WATERWAYS_SCENES)
SCENES.update(DEEPNEST_SCENES)
SCENES.update(ADMITTED)

EXPORT_ROOT = ROOT/'_private/research/crossroads-scenes'
REGIONS = ('Fungus1+Cliffs', 'Fungus2', 'Fungus3', 'Deepnest+Hive',
           'Mines+Waterways', 'Ruins', 'Abyss+RestingGrounds+Crossroads',
           'Room', 'Other', 'Deferred')
DEFERRED_PREFIXES = ('GG_', 'White_', 'Dream_', 'Grimm_')

def region_for(scene):
    if scene.startswith(DEFERRED_PREFIXES):
        return 'Deferred'
    for region, prefixes in (
        (REGIONS[0], ('Fungus1_', 'Cliffs_')),
        (REGIONS[1], ('Fungus2_',)),
        (REGIONS[2], ('Fungus3_',)),
        (REGIONS[3], ('Deepnest_', 'Hive_')),
        (REGIONS[4], ('Mines_', 'Waterways_')),
        (REGIONS[5], ('Ruins',)),
        (REGIONS[6], ('Abyss_', 'RestingGrounds_', 'Crossroads_')),
        (REGIONS[7], ('Room_',)),
    ):
        if scene.startswith(prefixes):
            return region
    return 'Other'


def inventory_scenes(path, *, regions=None, scenes=None, include_deferred=False):
    """Keep the inventory's authoritative indices; never infer them from names."""
    inventory = json.loads(Path(path).read_text(encoding='utf-8'))
    result = {}
    for row in inventory['rows']:
        if row['status'] != 'not_exported':
            continue
        name, index = row['scene'], row['index']
        if not isinstance(name, str) or not name or Path(name).name != name or name in ('.', '..'):
            raise ValueError(f'Invalid inventory scene name: {name!r}')
        if type(index) is not int or index < 0:
            raise ValueError(f'Invalid build index for {name}: {index!r}')
        if name in result:
            raise ValueError(f'Duplicate inventory scene: {name}')
        region = region_for(name)
        if region == 'Deferred' and not include_deferred:
            continue
        if regions and region not in regions:
            continue
        if scenes and name not in scenes:
            continue
        result[name] = index
    if scenes and set(scenes) - result.keys():
        raise ValueError(f'Scenes absent from selected not_exported inventory: {sorted(set(scenes) - result.keys())}')
    return dict(sorted(result.items(), key=lambda item: (REGIONS.index(region_for(item[0])), item[1])))


def read_json_object(path):
    try:
        value = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, ValueError, TypeError):
        return None
    return value if isinstance(value, dict) else None


def valid_terrain_header(value, scene):
    return (isinstance(value, dict) and value.get('scene') == scene
            and isinstance(value.get('inventory'), list)
            and isinstance(value.get('source_sha256'), dict))


def valid_visual_header(value, scene):
    return (isinstance(value, dict) and value.get('scene') == scene
            and isinstance(value.get('inventory'), list)
            and isinstance(value.get('sprites'), dict)
            and isinstance(value.get('source_sha256'), dict)
            and ('tilemap' not in value or isinstance(value['tilemap'], list)))


def valid_scene_manager(value):
    color = value.get('defaultColor') if isinstance(value, dict) else None
    intensity = value.get('defaultIntensity') if isinstance(value, dict) else None
    return (isinstance(intensity, (int, float))
            and isinstance(color, dict)
            and all(isinstance(color.get(channel), (int, float)) for channel in 'rgb'))


def scene_uses_borders(value):
    return (isinstance(value, dict) and isinstance(value.get('borderPrefab'), dict)
            and type(value.get('sceneBordersMask')) is int)


def valid_scene_camera(base):
    """A resumed export must retain the dimensions required by room borders."""
    try:
        return authored_camera_limits(base, base.name) is not None
    except (OSError, ValueError, KeyError, TypeError, AttributeError):
        return False


def source_assets_present(value, directory):
    """A saved header is resumable only while its referenced source files exist."""
    # Export writes flat asset names. One listing avoids thousands of stat calls.
    with os.scandir(directory) as entries:
        present = {entry.name for entry in entries if entry.is_file()}
    if not value['source_sha256'].keys() <= present:
        return False
    # Older terrain snapshots omitted non-colliding prompt children. Resume the
    # source export when Door Control's child lookup is not closed on disk.
    def read(key):
        return json.loads((directory/key).read_text(encoding='utf-8'))
    try:
        for row in value.get('inventory', []):
            groups = row.get('components', [])
            if not isinstance(groups, list) or not groups or 'TransitionPoint' not in groups[0]:
                continue
            for key in groups[0].get('PlayMakerFSM', []):
                component = read(key)
                fsm = component.get('m_Structure', {}).get('fsm', {})
                if component.get('m_Enabled', 1) and fsm.get('name') == 'Door Control':
                    binding = prompt_binding(fsm)
                    if binding:
                        prompt_marker(key, read, read(row['chain'][0]), binding['marker_name'])
    except (OSError, ValueError, KeyError, TypeError, IndexError):
        return False
    return True


def already_exported(base):
    """A partial export is resumable, but never counts as a completed scene."""
    try:
        terrain_map = read_json_object(base/'terrain/terrain-map.json')
        mapping = read_json_object(base/'visuals/room-visuals-map.json')
        manager = (base/'scene-manager.json').read_bytes()
        settings = json.loads(manager.decode('utf-8'))
        return (valid_terrain_header(terrain_map, base.name)
                and source_assets_present(terrain_map, base/'terrain')
                and valid_visual_header(mapping, base.name)
                and has_breakable_export(mapping)
                and source_assets_present(mapping, base/'visuals')
                and 'tilemap' in mapping
                and mapping.get('tilemap_checked') is True
                and valid_scene_manager(settings)
                and (not scene_uses_borders(settings)
                     or (valid_scene_borders(base, base.name)
                         and valid_scene_camera(base)))
                and mapping.get('scene_manager_sha256') == hashlib.sha256(manager).hexdigest())
    except (OSError, ValueError, KeyError, TypeError):
        return False


def extract(client, scene, index, output_root=None):
    base = output_root/scene if output_root is not None else ROOT/scene_source(scene)
    # Worktrees keep converted assets locally and junction research to the shared
    # private store. Check the resolved research root as well as the local root.
    if not any(base.resolve().is_relative_to(root.resolve())
               for root in (ROOT/'_private', ROOT/'_private/research')):
        raise ValueError('Scene exports must remain private')
    if (scene == 'Room_ruinhouse' and output_root is None and
            (base.is_symlink() or not base.resolve().is_relative_to(
                (ROOT/'_private/minimum-world-quests/scenes').resolve()))):
        raise ValueError('Ruinhouse source must stay in the owned scene export directory')
    base.mkdir(parents=True, exist_ok=True)
    terrain_path = base/'terrain/terrain-map.json'
    terrain_map = read_json_object(terrain_path)
    if not (valid_terrain_header(terrain_map, scene)
            and source_assets_present(terrain_map, base/'terrain')):
        terrain(client, base/'terrain', f'level{index}', scene)
    e = Export(client, base)
    manager_path = base/'scene-manager.json'
    settings = read_json_object(manager_path)
    if not valid_scene_manager(settings):
        settings = None
        for manager in client.search('SceneManager'):
            if manager['collection']!=f'level{index}' or manager['type']!='GameObject':
                continue
            parts = e.components(manager['path']['C'], manager['path']['D'])
            if parts.get('SceneManager'):
                candidate = e.cache[parts['SceneManager'][0]]['m_Structure']
                if valid_scene_manager(candidate):
                    settings = candidate
                    break
        if settings is None:
            raise ValueError(f'{scene}: no valid SceneManager; additive subscenes are not standalone rooms')
        manager_path.write_text(json.dumps(settings))
    visual_map = read_json_object(base/'visuals/room-visuals-map.json')
    if not (valid_visual_header(visual_map, scene)
            and source_assets_present(visual_map, base/'visuals')):
        visuals(client, base/'visuals', f'level{index}', scene)
    path = base/'visuals/room-visuals-map.json'
    mapping = read_json_object(path)
    if not valid_visual_header(mapping, scene):
        raise ValueError(f'Visual export produced an invalid header for {scene}')
    if not has_breakable_export(mapping):
        supplement_breakables(client, base/'visuals', f'level{index}')
        mapping = read_json_object(path)
        if not has_breakable_export(mapping):
            raise ValueError(f'Breakable export produced an incomplete bank for {scene}')
    if scene_uses_borders(settings) and not valid_scene_borders(base, scene):
        scene_borders(client, base, index)
    if 'tilemap' not in mapping:
        tiles=Export(client,base/'visuals')
        mapping['tilemap']=extract_tilemap(tiles,f'level{index}')
        mapping['source_sha256'].update(tiles.hashes)
    if 'blur_planes' not in mapping:
        from room_blur import extract_blur_planes
        blur_export = Export(client, base/'visuals')
        mapping['blur_planes'] = extract_blur_planes(blur_export, f'level{index}')
        mapping['source_sha256'].update(blur_export.hashes)
    mapping['tilemap_checked']=True
    intensity = 1+(settings['defaultIntensity']-1)*.5
    mapping['scene_ambient_rgb'] = [settings['defaultColor'][c]*intensity for c in 'rgb']
    mapping['scene_manager_sha256'] = hashlib.sha256((base/'scene-manager.json').read_bytes()).hexdigest()
    path.write_text(json.dumps(mapping, indent=2))
    scene_camera(client, base/'camera', f'level{index}', scene)
    print('EXPORTED', scene, flush=True)


def export_batch(client, scenes, *, root=EXPORT_ROOT, report=None, jobs=1):
    result = dict(selected=dict(scenes), exported=[], skipped=[], failed={})
    def export_one(item):
        scene, index = item
        try:
            base = root/scene if root is not None else ROOT/scene_source(scene)
            if already_exported(base):
                print('SKIPPED', scene, flush=True)
                return scene, 'skipped', None
            else:
                extract(client, scene, index, root)
                if not already_exported(base):
                    raise ValueError('Export did not produce a complete scene package')
                return scene, 'exported', None
        except Exception as error:
            failure = dict(build_index=index, error=f'{type(error).__name__}: {error}',
                           traceback=traceback.format_exc())
            print('FAILED', scene, failure['error'], flush=True)
            return scene, 'failed', failure
    # Only the coordinator writes reports; workers own separate scene directories.
    with ThreadPoolExecutor(max_workers=jobs) if jobs > 1 else nullcontext() as pool:
        outcomes = pool.map(export_one, scenes.items()) if pool else map(export_one, scenes.items())
        for scene, status, failure in outcomes:
            if failure:
                result['failed'][scene] = failure
            else:
                result[status].append(scene)
            if report:
                report.parent.mkdir(parents=True, exist_ok=True)
                temporary = report.with_suffix(report.suffix + '.tmp')
                temporary.write_text(json.dumps(result, indent=2), encoding='utf-8')
                temporary.replace(report)
    if report and not scenes:
        report.parent.mkdir(parents=True, exist_ok=True)
        report.write_text(json.dumps(result, indent=2), encoding='utf-8')
    return result


def main(argv=None):
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port', type=int, default=58292)
    p.add_argument('--inventory', type=Path,
                   help='Select not_exported scenes and build indices from this inventory')
    p.add_argument('--scene', action='append', help='Select a scene (repeatable)')
    p.add_argument('--output-root', type=Path, help='Private, checkout-owned scene export directory')
    p.add_argument('--region', action='append', choices=REGIONS)
    p.add_argument('--include-deferred', action='store_true')
    p.add_argument('--jobs', type=int, choices=(1, 2), default=2,
                   help='Independent scene exports within the locked region (default: 2)')
    p.add_argument('--report', type=Path, help='Resumable JSON results, including exact failures')
    p.add_argument('--progress-file', type=Path, help='Append a region completion line for waiting agents')
    p.add_argument('--list', action='store_true', help='Print the selection without contacting AssetRipper')
    a=p.parse_args(argv)
    if a.inventory:
        scenes = inventory_scenes(a.inventory, regions=a.region, scenes=a.scene,
                                  include_deferred=a.include_deferred)
        output_root = a.output_root if a.output_root is not None else EXPORT_ROOT
    else:
        if a.region or a.include_deferred:
            p.error('--region and --include-deferred require --inventory')
        candidates = SCENES | B2_CANDIDATES
        unknown = set(a.scene or ()) - candidates.keys()
        if unknown:
            p.error(f'Scenes require --inventory: {sorted(unknown)}')
        scenes = {scene: candidates[scene] for scene in a.scene} if a.scene else SCENES
        output_root = a.output_root
    if a.list:
        print(json.dumps(scenes, indent=2))
        return 0
    client = Client(a.port)
    # A region session is immutable: reuse the large collection-wide searches.
    client.request = lru_cache(maxsize=8192)(client.request)
    client.search = cache(client.search)
    result = export_batch(client, scenes, root=output_root, report=a.report, jobs=a.jobs)
    if a.progress_file:
        region = ', '.join(a.region) if a.region else 'all'
        line = (f"EXPORTED {region}: {len(result['exported']) + len(result['skipped'])} scenes "
                f"({len(result['skipped'])} already saved), failed: {list(result['failed'])}\n")
        with a.progress_file.open('a', encoding='utf-8') as stream:
            stream.write('\n' + line)
    return int(bool(result['failed']))


if __name__=='__main__':
    raise SystemExit(main())
