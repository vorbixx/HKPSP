"""Export the primary route rooms from the private Windows game copy."""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from functools import cache, lru_cache
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parent))
from export_steam_base import ROOT, game_session, prepare as publish_exports
import convert_false_knight_audio
import convert_crossroads_bosses
import extract_false_knight_effects
from extract_mantis_gate_art import recover as recover_gate_art
import extract_cornifer
import extract_stag
import export_steam_false_knight
from extract_scenes import already_exported, extract, read_json_object, source_assets_present, valid_terrain_header
from extract_terrain import extract as extract_terrain
from eastern_rooms import scene_source
from room_packages import DREAM_SCENES, census_itinerary
from tool_paths import TEX3DS as TEX3DS_EXE


GATE_OWNERS = {
    'Fungus2_14': {'level178-434.json', 'level178-2415.json'},
    'Fungus2_15': {'level179-247.json', 'level179-2722.json'},
    'Fungus2_18': {'level183-1248.json', 'level183-3194.json'},
    'Ruins1_05': {'level92-631.json', 'level92-382.json'},
    'Ruins1_05b': {'level93-539.json', 'level93-559.json'},
    'Ruins1_05c': {'level94-1555.json', 'level94-1385.json', 'level94-1493.json', 'level94-1258.json'},
}


def scene_indices(game_data):
    import UnityPy

    settings = next(obj.read_typetree() for obj in UnityPy.load(str(game_data / 'globalgamemanagers')).objects
                    if obj.type.name == 'BuildSettings')
    indices = {}
    for index, path in enumerate(settings['scenes']):
        scene = Path(path.replace('\\', '/')).stem
        if scene in indices:
            raise ValueError(f'Duplicate scene in original build settings: {scene}')
        indices[scene] = index
    return indices


# Waterways_05 reads the Dung Defender arena terrain from its additive scene.
# The other boss scenes hold the owners whose facing export_actor_facing.py
# reads (Hornet, Mantis Lords, Watcher Knights, False Knight).
SOURCE_SCENES = {'Waterways_05': ('Waterways_05_boss',), 'Fungus1_04': ('Fungus1_04_boss',),
                 'Fungus2_15': ('Fungus2_15_boss',), 'Ruins2_03': ('Ruins2_03_boss',),
                 'Crossroads_10': ('Crossroads_10_boss',)}
SOURCE_ONLY = {extra for extras in SOURCE_SCENES.values() for extra in extras}


def route_scenes():
    itinerary = census_itinerary(ROOT / 'docs/tasks/minimum-route.md')
    return tuple(dict.fromkeys(scene for rows in itinerary.values() for scene, status in rows
                               if (status != 'U' or scene in DREAM_SCENES) and scene != 'Tutorial_01'))


def prepare_false_knight_assets(game_data):
    packages = ROOT / '_private/tools/boss-python'
    effects = lambda _client, output: extract_false_knight_effects.extract(
        SimpleNamespace(source=game_data, output=output, packages=packages))
    audio = lambda _client, output: convert_false_knight_audio.extract(output, packages, game_data)
    publish_exports(None, ROOT / '_private/research', (('false-knight-effects', 'manifest.json', effects),))
    publish_exports(None, ROOT / '_private/converted/false-knight-audio',
                    (('source', 'source-hashes.json', audio),))
    destination = ROOT / '_private/false-knight-source-art'

    def source_art(_client, output):
        with tempfile.TemporaryDirectory(prefix='false-knight-build-', dir=output.parent) as temporary:
            arguments = SimpleNamespace(source=game_data, output=Path(temporary),
                packages=packages, tex3ds=Path(TEX3DS_EXE),
                scale=1, snapshot=None, save_snapshot=output, effects=None, false_knight_only=True)
            convert_crossroads_bosses.convert(arguments)

    publish_exports(None, destination, (('fullres', 'manifest.json', source_art),))


def prepare_gate_room(client, primary, target, owners):
    if target.resolve() != target:
        raise ValueError(f'Gate room export must stay within this checkout: {target}')
    if (target / 'visuals/tk2d/manifest.json').is_file():
        return
    if target.exists():
        raise ValueError(f'Incomplete gate room export already exists: {target}')
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.gate-room-', dir=target.parent) as temporary:
        pending = Path(temporary) / target.name
        recover_gate_art(client, primary / 'visuals', pending, owners=owners)
        if not (pending / 'visuals/tk2d/manifest.json').is_file():
            raise ValueError(f'Gate room export did not recover authored owners: {target}')
        pending.rename(target)


def prepare_route_resources(executable, game_data, scenes):
    research = ROOT / '_private/research'
    resources = []
    if 'Crossroads_33' in scenes and not (research / 'cornifer/cornifer-map.json').is_file():
        resources.append((64, ('cornifer', 'cornifer-map.json', extract_cornifer.extract)))
    if 'RestingGrounds_09' in scenes and not (research / 'stag-stations/stag-map.json').is_file():
        resources.append((9, ('stag-stations', 'stag-map.json', extract_stag.extract_loaded)))
    if resources:
        with game_session(executable, game_data, [level for level, _ in resources], 'assetripper-route-resources.log') as client:
            publish_exports(client, research, tuple(row for _, row in resources))


def prepare(executable, game_data, scenes, jobs=1):
    output = ROOT / '_private/research/crossroads-scenes'
    if output.resolve() != output:
        raise ValueError(f'Route exports require a checkout-local research directory: {output}')
    indices = scene_indices(game_data)
    missing = set(scenes) - indices.keys()
    if missing:
        raise ValueError(f'Rooms absent from original build settings: {", ".join(sorted(missing))}')
    prepare_route_resources(executable, game_data, scenes)

    def export_source_terrain(scene):
        # An additive scene has no SceneManager of its own; its room reads
        # only the additive terrain (Dung Defender's arena transforms).
        base = output / scene / 'terrain'
        mapping = read_json_object(base / 'terrain-map.json')
        if valid_terrain_header(mapping, scene) and source_assets_present(mapping, base):
            print(f'{scene}: existing terrain export', flush=True)
            return
        with game_session(executable, game_data, indices[scene], f'assetripper-{scene}.log') as client:
            extract_terrain(client, base, f'level{indices[scene]}', scene)

    def export_scene(scene):
        if scene in SOURCE_ONLY:
            return export_source_terrain(scene)
        canonical = (Path('_private/converted-psp/route-scenes-source') / scene
                     if scene in DREAM_SCENES else scene_source(scene))
        # These rooms have dedicated source roots whose directory still has
        # the authored scene name. Gate-specific variant rooms retain their
        # separate recovery step after the primary export.
        room_root = ROOT / canonical.parent if canonical.name == scene else output
        target = room_root / scene
        if target.resolve() != target:
            raise ValueError(f'Route scene output is linked outside its expected path: {target}')
        complete = already_exported(target)
        owners = GATE_OWNERS.get(scene)
        variant = ROOT / canonical
        recover_gate = owners and not (variant / 'visuals/tk2d/manifest.json').is_file()
        if complete:
            print(f'{scene}: existing export', flush=True)
        if not complete or recover_gate:
            print(f'{scene}: preparing level{indices[scene]}', flush=True)
            with game_session(executable, game_data, indices[scene], f'assetripper-{scene}.log') as client:
                client.search = cache(client.search)
                client.request = lru_cache(maxsize=8192)(client.request)
                if not complete:
                    extract(client, scene, indices[scene], room_root)
                if recover_gate:
                    prepare_gate_room(client, target, variant, owners)
            if not already_exported(target):
                raise ValueError(f'Scene export is incomplete: {target}')
        if scene == 'Crossroads_10':
            additive = target / export_steam_false_knight.SNAPSHOT
            visual_map = target / 'visuals/level48/room-visuals-map.json'
            if not additive.is_file() or not visual_map.is_file():
                with game_session(executable, game_data, 48, 'assetripper-false-knight.log') as client:
                    export_steam_false_knight.prepare(client, game_data, target)
            prepare_false_knight_assets(game_data)

    # Each scene loads its own AssetRipper session on a free port, so several
    # rooms can export at once; every room writes only its own directory.
    if jobs <= 1:
        for scene in scenes:
            export_scene(scene)
        return
    failures = {}
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = {pool.submit(export_scene, scene): scene for scene in scenes}
        for future in as_completed(futures):
            try:
                future.result()
            except Exception as error:  # Report every failed room, not just the first.
                failures[futures[future]] = error
                print(f'{futures[future]}: FAILED {error!r}', flush=True)
    if failures:
        raise RuntimeError(f'{len(failures)} scene exports failed: {", ".join(sorted(failures))}')


def all_rooms():
    """Every PSP room (tools/psp/all-rooms.txt) plus the route's dream scenes."""
    listed = [line.strip() for line in (ROOT / 'tools/psp/all-rooms.txt').read_text(encoding='utf-8').splitlines()
              if line.strip() and not line.startswith('#')]
    return tuple(dict.fromkeys(scene for scene in (*listed, *route_scenes()) if scene != 'Tutorial_01'))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    listing = '--list-rooms' in sys.argv
    parser.add_argument('--assetripper', type=Path, required=not listing)
    parser.add_argument('--game-data', type=Path, required=not listing)
    parser.add_argument('--scene', action='append', help='Limit extraction to the named scene, repeatable')
    parser.add_argument('--all-rooms', action='store_true', help='Export every PSP room, not only the route')
    parser.add_argument('--jobs', type=int, default=1, help='Rooms exported at once (one AssetRipper each)')
    parser.add_argument('--list-rooms', action='store_true', help='Print every packaged room and exit')
    args = parser.parse_args()
    if args.list_rooms:
        print(chr(10).join(('Tutorial_01', *all_rooms())))
        raise SystemExit(0)
    scenes = args.scene or (all_rooms() if args.all_rooms else route_scenes())
    # Source-only scenes that a room's conversion reads but that are not rooms.
    scenes = tuple(dict.fromkeys(extra for scene in scenes
                                 for extra in (scene, *SOURCE_SCENES.get(scene, ()))))
    prepare(args.assetripper, args.game_data, scenes, max(1, args.jobs))
