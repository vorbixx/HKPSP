"""Prepare the hero, HUD and King's Pass inputs from a loaded local export session."""
import argparse
from contextlib import contextmanager
from functools import cache, partial
import html
import json
from pathlib import Path
import re
import socket
import subprocess
import sys
import tempfile
import time
from urllib.parse import urlencode
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[2]
# The driver runs Python with -I, which leaves the script folder off sys.path.
sys.path.insert(0, str(ROOT / 'tools/psp'))
sys.path.insert(0, str(ROOT / 'tools'))
from assetripper_client import Client
import convert_ability_pickups
import convert_enemy_audio
import convert_inventory
import convert_music_audio
import convert_tutorial_audio
import convert_wind_audio
import extract_area_title
import extract_buzzer
import extract_crawler
import extract_focus_effects
import extract_fury_shiny
import extract_fury_tutorial
import extract_geo_visuals
import extract_great_door_motion
import extract_great_door_visuals
import extract_health_scuttler
import extract_hero
import extract_hero_damage_flash
import extract_hero_death
import extract_hero_effects
import extract_hud
import extract_lore_tablets
import extract_nail_interactions
import extract_opening_credits
import extract_room_visuals
import extract_stalactite_sources
import extract_tablet_glow
import extract_terrain
import extract_tutorial_prompts
import extract_steam_tutorial
import export_steam_lighting
from restore_tutorial_research import restore
import restore_fury_research
import restore_shade_research


def game_data_root():
    return ROOT / '_private/original/Hollow Knight/hollow_knight_Data'


def export_shade(client, output):
    restore_shade_research.restore(client=client, output=output,
        assembly=game_data_root() / 'Managed/Assembly-CSharp.dll')


def export_fury(client, output, group):
    restore_fury_research.restore(None, group, client=client, output=output,
        assembly=game_data_root() / 'Managed/Assembly-CSharp.dll')
    if group == 'fury-chest':
        extract_fury_shiny.extract(client, output)


def export_interactions(client, output):
    extract_nail_interactions.extract(client, output, game_data_root())


def export_fury_reveal(client, output):
    extract_fury_tutorial.extract(client, output, output.parent.parent / 'fury-chest')


def export_geo(client, output):
    from extract_crawler import Export
    original = [row for row in client.search('Geo Small') if row['name'] == 'Geo Small'
                and row['type'] == 'GameObject' and row['collection'] == 'resources.assets']
    if len(original) != 1:
        raise ValueError('Expected one original small Geo prefab')
    row = original[0]
    collection = row['path']['C']
    export = Export(client, output)
    parts = export.components(collection, row['path']['D'])
    box = export.cache[parts['BoxCollider2D'][0]]
    _, material, _ = export.resolve(collection, box['m_Material'])
    manifest = dict(build_id=22529139, parts=parts, material=material, sha256=export.hashes)
    (output / 'sources.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')


def export_geo_art(client, output):
    extract_geo_visuals.extract(client, output)
    extract_health_scuttler.extract(client, output)


def extract_damage_flash(client, output):
    extract_hero_damage_flash.extract(client=client, output=output, include_flings=False)


def extract_damage_flings(client, output):
    dispatch = ROOT / '_private/research/hero-damage-flash/dispatch.json'
    extract_hero_damage_flash.extract_flings(client, extract_hero_damage_flash.damage_collection(client),
                                           json.loads(dispatch.read_text(encoding='utf-8')), output)


def extract_tutorial(client, output):
    # Output sits in a temporary sibling of the published terrain export.
    extract_steam_tutorial.extract(client, output, output.parent.parent / 'terrain')


def tutorial_sidecar(_client, output, name):
    restore(ROOT, name, output)


def extract_entity_library(client, output):
    _, _, library, _ = extract_buzzer.resolve_library(client, output, output.parent.parent / 'tutorial')
    if library != 'sharedassets6.assets-1150.json':
        raise ValueError(f'Unexpected Tutorial_01 Buzzer library: {library}')


def extract_titles(client, output):
    extract_area_title.extract(client, output)
    extract_area_title.extract_large(client, output)


def extract_inventory(client, output):
    convert_inventory.extract(output, client)


def extract_ability_art(client, output):
    convert_ability_pickups.extract(output, client=client, include_shade_soul=False)


ABILITY_EXPORT = ('ability-pickups', 'manifest.json', extract_ability_art)
SHADE_SOUL_EXPORT = ('art', 'fireball2-spiral-summary.json', convert_ability_pickups.extract_shade_soul)


EXPORTS = (
    ('assets', 'export-map.json', extract_hero.extract),
    ('hud', 'hud-map.json', extract_hud.extract),
    ('hud-layout', 'layout-map.json', extract_hud.extract_layout),
    ('hud-liquid', 'liquid-map.json', extract_hud.extract_liquid),
    ('hud-feedback', 'feedback-map.json', extract_hud.extract_feedback),
    ('hud-limiter', 'limiter-map.json', extract_hud.extract_limiter),
    ('inventory', 'inventory-map.json', extract_inventory),
    ('hero-death', 'death-map.json', extract_hero_death.extract),
    ('hero-effects', 'hero-effects.json', extract_hero_effects.extract),
    ('focus-effects', 'focus-effects.json', extract_focus_effects.extract),
    ('hero-light', 'lighting-export.json', export_steam_lighting.extract),
    ('hero-light-donut', 'lighting-export.json', export_steam_lighting.extract_donut),
    ('hero-damage-flash', 'damage-flash.json', extract_damage_flash),
    ('hero-damage-flings', 'damage-flings.json', extract_damage_flings),
    ('terrain', 'terrain-map.json', extract_terrain.extract),
    ('tutorial', 'inventory.json', extract_tutorial),
    ('room-entities', 'sharedassets6.assets-1150.json', extract_entity_library),
    ('enemy', 'crawler-map.json', extract_crawler.extract),
    ('buzzer', 'buzzer-map.json', extract_buzzer.extract),
    ('room-visuals', 'room-visuals-map.json', extract_room_visuals.extract),
    ('fury-mask', 'manifest.json', partial(tutorial_sidecar, name='fury-mask')),
    ('hollow-shade', 'shade-map.json', export_shade),
    ('nail-interactions', 'interaction-map.json', export_interactions),
    ('fury-chest', 'fury-chest-map.json', partial(export_fury, group='fury-chest')),
    ('fury-presentation-source-20261022', 'fury-presentation-map.json',
     partial(export_fury, group='fury-presentation-source-20261022')),
    ('fury-reveals-source-20261023', 'manifest.json', export_fury_reveal),
    ('great-door', 'sources.json', partial(tutorial_sidecar, name='great-door')),
    ('great-door-motion-source', 'motion-source-map.json', extract_great_door_motion.extract),
    ('great-door-visuals', 'great-door-visuals-map.json', extract_great_door_visuals.extract),
    ('stalactite-sources', 'stalactite-sources.json', extract_stalactite_sources.extract),
    ('geo', 'sources.json', export_geo),
    ('geo-visuals', 'geo-visuals-map.json', export_geo_art),
    ('tutorial-reminders', 'prompt-map.json', extract_tutorial_prompts.extract),
    ('opening-credits', 'credits-map.json', extract_opening_credits.extract),
    ('lore-tablets', 'lore-map.json', extract_lore_tablets.extract),
    ('tablet-glow', 'tablet-glow-map.json', extract_tablet_glow.extract),
    ('area-title', 'large-title-map.json', extract_titles),
)


def export_title_source(client, output):
    import convert_title_menu
    original = ROOT / '_private/original/Hollow Knight/hollow_knight_Data'
    convert_title_menu.extract(output, original, client=client)
    convert_title_menu.extract_menu_arrow(output, original)
    convert_title_menu.extract_menu_particles(output, original)


def prepare(client, research, exports=None):
    if research.resolve() != research:
        raise ValueError(f'Base exports require a checkout-local research directory: {research}')
    research.mkdir(parents=True, exist_ok=True)
    for name, manifest, extract in EXPORTS if exports is None else exports:
        target = research / name
        if target.resolve() != target:
            raise ValueError(f'Export destination is linked outside its expected path: {target}')
        if (target / manifest).is_file():
            print(f'{name}: existing export', flush=True)
            continue
        lighting_upgrade = name in ('hero-light', 'hero-light-donut') and target.is_dir()
        if target.exists() and not lighting_upgrade:
            raise ValueError(f'Incomplete export at {target}. Keep it separately before retrying.')
        print(f'{name}: exporting', flush=True)
        with tempfile.TemporaryDirectory(prefix=f'.export-{name}-', dir=research) as temporary:
            pending = Path(temporary) / name
            extract(client, pending)
            if not (pending / manifest).is_file():
                raise ValueError(f'{name}: extractor did not write {manifest}')
            if lighting_upgrade:
                # Earlier local builds already exported ambient light. Fill in
                # missing files and keep any existing reviewed source records.
                for asset in pending.iterdir():
                    destination = target / asset.name
                    if destination.resolve() != destination:
                        raise ValueError(f'Lighting export destination is linked: {destination}')
                    if destination.exists() and not destination.is_file():
                        raise ValueError(f'Lighting export requires a file: {destination}')
                for asset in pending.iterdir():
                    if asset.name != manifest and not (target / asset.name).exists():
                        asset.rename(target / asset.name)
                (pending / manifest).rename(target / manifest)
            else:
                pending.rename(target)


def collection_path(client, name):
    """Find a loaded collection by name; indices are session-specific."""
    for index in range(1024):
        path = {'B': {'P': []}, 'I': index}
        try:
            page = client.request('/Collections/View', Path=json.dumps(path)).decode()
        except OSError:
            break
        if html.unescape(re.search(r'<title>(.*?)</title>', page)[1]) == name:
            return path
    raise ValueError(f'Load the original {name} first')


@contextmanager
def game_session(executable, game_data, level, log_name, port=None):
    """Load the scene collections into a private AssetRipper; `port` serves
    exporters that connect to a fixed address."""
    executable = executable.resolve(strict=True)
    game_data = game_data.resolve(strict=True)
    logs = ROOT / 'build/psp/steam-build'
    logs.mkdir(parents=True, exist_ok=True)
    paths = session_files(game_data, level)
    paths.extend((game_data / 'Managed', game_data / 'Resources'))
    if port is None:
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            port = listener.getsockname()[1]
    log_path = logs / log_name
    with log_path.open('wb') as log:
        # AssetRipper names its own log by the second, beside the executable:
        # two parallel sessions started in one second crashed on the shared
        # file (Fungus2_15_boss, 2026-11-25). Each session gets its own log.
        process = subprocess.Popen([str(executable), '--headless', '--port', str(port),
                                    '--log-path', str(log_path.with_suffix('.internal.log'))],
                                   cwd=logs, stdout=log, stderr=subprocess.STDOUT,
                                   creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        try:
            client = Client(port)
            deadline = time.monotonic() + 60
            while True:
                if process.poll() is not None:
                    raise RuntimeError(f'AssetRipper exited. See {log_path}')
                try:
                    with urlopen(client.base + '/', timeout=5):
                        break
                except OSError:
                    if time.monotonic() >= deadline:
                        raise
                    time.sleep(1)
            # LoadFile accepts a repeated Path field. The read-only inputs
            # stay in place even when the archive lives on another drive.
            request = Request(client.base + '/LoadFile',
                              data=urlencode([('Path', str(path)) for path in paths]).encode())
            # 2026-11-26: a WSL run reading the game over /mnt/f took over 20 min at 8 % CPU;
            # a player's hard disk can be as slow. An hour still catches a hung AssetRipper.
            with urlopen(request, timeout=3600) as response:
                response.read()
            yield client
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=10)


@cache
def shared_references(path):
    import UnityPy
    file = UnityPy.load(str(path)).file
    return tuple(Path(reference.path.replace('\\', '/')).name for reference in file.externals
                 if reference.path.endswith('.assets'))


def session_files(game_data, level):
    """Include transitive material, shader, and prefab dependencies at load time."""
    levels = (level,) if isinstance(level, int) else tuple(level)
    pending = [*(f'level{index}' for index in levels), 'globalgamemanagers.assets',
               'resources.assets', 'sharedassets0.assets', 'sharedassets1.assets']
    pending.extend(name for index in levels if (game_data / (name := f'sharedassets{index}.assets')).is_file())
    collections = set()
    while pending:
        name = pending.pop()
        if name in collections:
            continue
        path = game_data / name
        if not path.is_file():
            raise FileNotFoundError(f'Missing Unity collection dependency: {path}')
        collections.add(name)
        pending.extend(shared_references(path))
    # AssetRipper resolves streaming data (.resS/.resource) from sibling files.
    prefixes = tuple(name.removesuffix('.assets') + '.' for name in collections
                     if name.endswith('.assets'))
    return sorted(path for path in game_data.iterdir()
                  if path.is_file() and (path.name in collections
                                         or path.name == 'globalgamemanagers'
                                         or path.name.startswith(prefixes)))


def prepare_from_game(executable, game_data, research, exports=None):
    if research.resolve() != research:
        raise ValueError(f'Base exports require a checkout-local research directory: {research}')
    exports = EXPORTS if exports is None else exports
    if all((research / name / manifest).is_file() for name, manifest, _ in exports):
        print('Base exports already exist', flush=True)
        return
    with game_session(executable, game_data, 6, 'assetripper-base.log') as client:
        prepare(client, research, exports)


def prepare_abilities(research, client=None, executable=None, game_data=None):
    spell_source = research.parent / 'minimum-abilities/spell-source'
    if ((research / 'ability-pickups/manifest.json').is_file()
            and (spell_source / 'art/fireball2-spiral-summary.json').is_file()):
        return
    if executable:
        with game_session(executable, game_data, (76, 132), 'assetripper-abilities.log') as session:
            prepare_abilities(research, session)
        return
    prepare(client, research, (ABILITY_EXPORT,))
    prepare(client, spell_source, (SHADE_SOUL_EXPORT,))


def prepare_audio(research, client=None, executable=None, game_data=None):
    game_data = game_data or game_data_root()
    converted = research.parent / 'converted'
    packages = ROOT / '_private/tools/audio-python'
    prepare(None, converted / 'music', (('source', 'source-hashes.json',
        lambda _client, output: convert_music_audio.extract_sources(output, game_data, packages)),))
    enemy = converted / 'enemy/source'
    wind = converted / 'wind/source'
    for directory in (enemy, wind):
        if directory.resolve() != directory:
            raise ValueError(f'Audio exports require a direct directory: {directory}')
    enemy_ready = all((enemy / f"{collection.replace('.', '-')}-{identity}-{name}.ogg").is_file()
                      for collection, identity, name, *_ in convert_enemy_audio.CLIPS)
    wind_ready = all(convert_wind_audio.source_path(wind, identity, name).is_file()
                     for identity, name in convert_wind_audio.CLIPS)
    tutorial_audio = (
        ('tutorial-audio', 'audio-map.json',
         lambda session, output: convert_tutorial_audio.convert(client=session, out=output)),
        ('plant-audio', 'audio-map.json',
         lambda session, output: convert_tutorial_audio.convert_plant(client=session, out=output)),
    )
    tutorial_ready = all((research / name / manifest).is_file() for name, manifest, _ in tutorial_audio)
    if enemy_ready and wind_ready and tutorial_ready:
        return
    if executable:
        with game_session(executable, game_data, (6, 7), 'assetripper-audio.log') as session:
            prepare_audio(research, session, game_data=game_data)
        return
    prepare(client, research, tutorial_audio)
    if not wind_ready:
        convert_wind_audio.extract_sources(wind, client=client)
    if not enemy_ready:
        convert_enemy_audio.extract_sources(enemy, client=client)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    session = parser.add_mutually_exclusive_group(required=True)
    session.add_argument('--port', type=int)
    session.add_argument('--assetripper', type=Path)
    parser.add_argument('--game-data', type=Path)
    parser.add_argument('--title-source', action='store_true')
    args = parser.parse_args()
    if bool(args.assetripper) != bool(args.game_data):
        parser.error('--assetripper and --game-data must be given together')
    research = ROOT / '_private/research'
    exports = EXPORTS
    if args.title_source:
        exports += (('title-menu', 'menu-particles.json', export_title_source),)
    if args.assetripper:
        prepare_from_game(args.assetripper, args.game_data, research, exports)
        prepare_abilities(research, executable=args.assetripper, game_data=args.game_data)
        prepare_audio(research, executable=args.assetripper, game_data=args.game_data)
    else:
        client = Client(args.port)
        prepare(client, research, exports)
        prepare_abilities(research, client)
        prepare_audio(research, client)
