"""Run the remaining source exporters against the Steam files.

The base and route exports cover the hero, HUD, title and room scenes. The
converters also read about eighty smaller research snapshots (bosses, enemies,
effects, obstacles). Each was produced by its own exporter in `tools/`; this
module runs them in order after the route export so a fresh checkout needs no
prepared `_private` tree. A job that finished leaves a stamp, so a failed build
resumes with the remaining jobs.

Job kinds:
- `unitypy`: reads `_private/original` directly.
- `session`: needs an AssetRipper session with the listed scenes loaded; the
  driver starts it on the port the exporter expects.
- `isolated`: the exporter starts its own AssetRipper (`HKPSP_ASSETRIPPER`).
"""
import argparse
from dataclasses import dataclass, field
import os
import tempfile
import shutil
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/psp'))
sys.path.insert(0, str(ROOT / 'tools'))
from export_steam_base import game_session

DEFAULT_PORT = 58291  # assetripper_client.Client() default


@dataclass(frozen=True)
class Job:
    name: str
    kind: str
    command: tuple
    scenes: tuple = field(default=())
    port: int = DEFAULT_PORT


def session(name, script, *args, scenes=('Tutorial_01',), port=DEFAULT_PORT):
    return Job(name, 'session', (script, *args), tuple(scenes), port)


def unitypy(name, script, *args):
    return Job(name, 'unitypy', (script, *args))


def isolated(name, script, *args):
    return Job(name, 'isolated', (script, *args))


# Rooms whose hazard respawn markers the scene rules read.
HAZARD_SCENES = ('Crossroads_11_alt', 'Fungus1_01', 'Fungus1_02', 'Fungus1_03', 'Fungus1_04', 'Fungus1_06',
                 'Fungus1_07', 'Fungus1_17')

JOBS = (
    # Tutorial_01 (level6) session: ambient, decor, plants, map and events.
    session('ambient-particles', 'tools/extract_ambient_particles.py'),
    *(session(f'fungus-particles-zone{zone}', 'tools/extract_ambient_particles.py', '--zone', str(zone),
              '--output', f'_private/fungus-particles/zone{zone}') for zone in (6, 7, 8, 9)),
    session('deepnest-ambient-particles', 'tools/psp/export_deepnest_ambient.py', '--port', '{port}'),
    session('conditional-events', 'tools/extract_conditional_events.py', '--port', '{port}'),
    session('decor-effects', 'tools/extract_decor_effects.py', '--port', '{port}',
            scenes=('Tutorial_01', 'Menu_Title')),
    session('initial-fall', 'tools/extract_initial_fall.py'),
    session('bottom-map', 'tools/extract_map.py'),
    session('opening-blanker', 'tools/extract_opening_blanker.py'),
    session('plants', 'tools/extract_plants.py', '--port', '{port}'),
    session('plant-effects', 'tools/extract_plant_effects.py', '--port', '{port}'),
    unitypy('plant-dependencies', 'tools/analyze_plants.py'),
    session('grass-artwork-source', 'tools/psp/export_grass_source.py', '--port', '{port}'),
    session('room-dynamics', 'tools/extract_room_dynamics.py'),
    session('tutorial-borders', 'tools/psp/export_tutorial_borders.py', '--port', '{port}'),
    session('breakable-owner-effects', 'tools/breakable_owner_effects.py', '--extract', '--port', '{port}'),
    # The crumble export reads the breakable owner manifest above.
    session('collapse-crumble', 'tools/extract_room_dynamics.py', '--crumble'),
    *(session(f'decor-research-{group}', 'tools/restore_decor_research.py', '--only', group,
              '--port', '{port}') for group in ('grass', 'door', 'contact')),
    # Quest owners and enemies in their own rooms.
    session('sly-quest-art', 'tools/world_quest_art.py', '--quest', 'sly', '--extract', '--port', '{port}',
            scenes=('Room_ruinhouse',)),
    session('bretta-quest-art', 'tools/world_quest_art.py', '--quest', 'bretta', '--extract',
            '--port', '{port}', scenes=('Fungus2_23',)),
    session('crossroads-enemies-a', 'tools/restore_crossroads_enemy_research.py', '--only', 'enemy-a',
            '--source', '_private/original/Hollow Knight/hollow_knight_Data', '--port', '{port}',
            scenes=('Crossroads_01', 'Crossroads_07', 'Crossroads_04', 'Crossroads_19')),
    session('crossroads-enemies-b', 'tools/restore_crossroads_enemy_research.py', '--only', 'enemy-b',
            '--source', '_private/original/Hollow Knight/hollow_knight_Data', '--port', '{port}',
            scenes=('Crossroads_01', 'Crossroads_04', 'Crossroads_13', 'Crossroads_19')),
    session('crossroads-enemies-c', 'tools/psp/export_crossroads_enemies_c.py', '--port', '{port}',
            scenes=(40, 45, 50, 57, 59, 65)),
    session('mantis-gate-art', 'tools/psp/export_mantis_gate_art.py', '--port', '{port}', scenes=('Fungus2_04',)),
    # Obstacles whose exporters start their own isolated session.
    isolated('city-crest-source', 'tools/extract_city_crest_respawn.py'),
    isolated('city-crest-slam-art', 'tools/extract_city_crest_slam_art.py'),
    isolated('city-lift-art', 'tools/extract_city_lift_art.py'),
    *(isolated(f'respawn-markers-{scene}', 'tools/extract_hazard_respawns.py', '--scene', scene)
      for scene in HAZARD_SCENES),
    # The Crossroads_16 scene rule reads its markers from the route source tree.
    isolated('respawn-markers-Crossroads_16', 'tools/extract_hazard_respawns.py', '--scene', 'Crossroads_16',
             '--output', '_private/converted-psp/route-scenes-source/Crossroads_16-markers'),
    isolated('mantis-18-room', 'tools/extract_mantis_gate_room.py'),
    isolated('ruins-gate-room', 'tools/extract_ruins_gate_art.py', '--scene', 'Ruins1_05b'),
    isolated('ruins-gate-05-room', 'tools/extract_ruins_gate_art.py', '--scene', 'Ruins1_05'),
    isolated('ruins-gate-05c-room', 'tools/extract_ruins_gate_art.py', '--scene', 'Ruins1_05c'),
    isolated('stag-station-art', 'tools/extract_stag_station_art.py', '--scene', 'Crossroads_47'),
    isolated('resting-station-art', 'tools/extract_stag_station_art.py', '--scene', 'RestingGrounds_09'),
    isolated('town-lever-feedback', 'tools/extract_town_lever_feedback.py'),
    isolated('waterways-art', 'tools/extract_waterways_art.py'),
    isolated('town-art', 'tools/extract_world_lift.py'),
    isolated('switch-art', 'tools/extract_world_switch_session.py'),
    # Bosses, obstacles and effects read directly with UnityPy.
    unitypy('ambient-particle-mesh', 'tools/extract_ambient_particles.py', '--mesh-only'),
    unitypy('resting-station-feedback', 'tools/extract_stag_feedback.py'),
    unitypy('city-crest-audio', 'tools/extract_city_crest_audio.py'),
    unitypy('city-crest-effects', 'tools/extract_city_crest_effects.py'),
    unitypy('city-crest-rocks', 'tools/extract_city_crest_effects.py', '--rocks'),
    unitypy('mantis-scenery', 'tools/extract_mantis_scenery.py'),
    unitypy('lantern-glow', 'tools/export_lantern_glow.py'),
    unitypy('hornet-protector', 'tools/export_hornet_source.py'),
    unitypy('battle-enemies', 'tools/export_battle_enemies.py'),
    unitypy('shaman-pillar-feedback', 'tools/psp/export_shaman_pillar.py'),
    unitypy('sly-shop-icons', 'tools/psp/export_sly_shop_icons.py'),
    # English sheets (the text pack) and Russian sheets and fonts (font fallback).
    unitypy('language-sources', 'tools/psp/export_language_sources.py'),
    *(unitypy(f'watcher-source-{scene}', 'tools/export_watcher_source.py', '--scene', scene)
      for scene in ('level113', 'level115')),
    unitypy('watcher-art', 'tools/convert_watcher_art.py', '--extract'),
    unitypy('broken-vessel-source', 'tools/export_broken_vessel_source.py'),
    unitypy('broken-vessel-art', 'tools/convert_broken_vessel_art.py', '--extract'),
    *(unitypy(f'uumuu-source-{scene}', 'tools/export_uumuu_source.py', '--scene', scene)
      for scene in ('level223', 'level228', 'level229', 'level402', 'level403')),
    *(unitypy(f'uumuu-prefab-{prefab}', 'tools/export_uumuu_source.py', '--prefab', prefab)
      for prefab in ('zap', 'corpse')),
    # The Monomon art and mask exporters read the Uumuu scene sources above.
    unitypy('monomon-guardian-art', 'tools/convert_monomon_art.py', '--extract'),
    unitypy('monomon-archive-art', 'tools/convert_monomon_art.py', '--extract', '--archive'),
    unitypy('monomon-mask', 'tools/convert_monomon_mask.py', '--extract'),
    unitypy('monomon-presentation', 'tools/convert_monomon_presentation.py', '--extract'),
    unitypy('uumuu-audio', 'tools/convert_uumuu_audio.py', '--extract'),
    unitypy('uumuu-body-art', 'tools/convert_uumuu_body_art.py', '--extract'),
    unitypy('uumuu-effects-art', 'tools/convert_uumuu_effects_art.py', '--extract'),
    unitypy('uumuu-gate-art', 'tools/convert_uumuu_gate_art.py', '--extract'),
    unitypy('uumuu-quirrel-art', 'tools/convert_uumuu_quirrel_art.py', '--extract'),
    unitypy('crossroads-bosses', 'tools/convert_crossroads_bosses.py', '--named-clips-only',
            '--save-snapshot', '_private/research/crossroads-bosses'),
    unitypy('dung-defender', 'tools/convert_dung_defender.py', '--save-snapshot', '_private/research/dung-defender'),
    unitypy('greenpath-enemies', 'tools/convert_greenpath_enemies.py', '--extract', '--no-romfs-copy'),
)


# Older exporters read the game from these private input folders. They become
# junctions to the same read-only game data instead of copies.
LEGACY_INPUTS = ('_private/analysis-input/hollow_knight_Data',
                 '_private/converted/research-input/hollow_knight_Data')


def link_legacy_inputs(game_data):
    target = game_data.resolve(strict=True)
    for relative in LEGACY_INPUTS:
        link = ROOT / relative
        # A junction made by another Windows account is an untrusted mount point on Windows 11 24H2
        # (WinError 448, laptop 2026-11-27): exists() raises or the folder cannot be listed. Recreate
        # only the link for this account; the game files it points at are never touched.
        try:
            if link.exists():
                next(link.iterdir(), None)
                continue
        except OSError:
            print(f'Recreating the link to the game data for this account: {link}', flush=True)
            os.rmdir(link)
        if os.path.lexists(link):
            os.rmdir(link)
        link.parent.mkdir(parents=True, exist_ok=True)
        if os.name == 'nt':
            # A junction needs no Developer Mode or administrator rights.
            import _winapi
            _winapi.CreateJunction(str(target), str(link))
        else:
            link.symlink_to(target, target_is_directory=True)


def scene_levels(game_data, scenes):
    from export_steam_route import scene_indices
    indices = scene_indices(game_data)
    # Scene names resolve through the build settings; integers are level indices.
    return tuple(scene if isinstance(scene, int) else indices[scene] for scene in scenes)


def mirror_mantis_scenery():
    """Give the Fungus2_15 gate room its level180 arena scenery.

    The route export copies Fungus2_15 into _private/minimum-obstacles/mantis-15-room before
    the mantis-scenery job writes visuals/level180 into the research scene, and scene-Fungus2_15
    reads level180 from the gate room (eastern_rooms.scene_source). Without this copy the player
    build stopped at scene-Fungus2_15 (FileNotFoundError on level180/room-visuals-map.json)."""
    source = ROOT / '_private/research/crossroads-scenes/Fungus2_15/visuals/level180'
    room = ROOT / '_private/minimum-obstacles/mantis-15-room/visuals'
    target = room / 'level180'
    if not room.is_dir() or not (source / 'room-visuals-map.json').is_file():
        return
    if (target / 'room-visuals-map.json').is_file():
        return
    with tempfile.TemporaryDirectory(prefix='.level180-', dir=room) as temporary:
        pending = Path(temporary) / 'level180'
        shutil.copytree(source, pending)
        if target.exists():
            shutil.rmtree(target)
        pending.rename(target)
    print(f'Copied Mantis arena scenery into {target.relative_to(ROOT)}', flush=True)


def run_job(job, python, environment, log):
    # Same interpreter flags as the regeneration rules: the script folder stays
    # on sys.path for the exporters' sibling imports.
    command = [python, '-E', *(part.replace('{port}', str(job.port)) for part in job.command)]
    print(f'{job.name}: {" ".join(command[2:])}', flush=True)
    with log.open('ab') as stream:
        stream.write(f'\n== {job.name}: {" ".join(command[2:])}\n'.encode())
        stream.flush()
        return subprocess.run(command, cwd=ROOT, env=environment, stdout=stream,
                              stderr=subprocess.STDOUT).returncode


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--assetripper', type=Path, required=True)
    parser.add_argument('--game-data', type=Path, required=True)
    parser.add_argument('--only', nargs='+', help='Run only these job names')
    parser.add_argument('--list', action='store_true')
    args = parser.parse_args(argv)
    if args.list:
        for job in JOBS:
            print(f'{job.name:32} {job.kind:9} {" ".join(map(str, job.command))}')
        return 0
    link_legacy_inputs(args.game_data)
    stamps = ROOT / 'build/psp/steam-build/extras-done'
    stamps.mkdir(parents=True, exist_ok=True)
    log = ROOT / 'build/psp/steam-build/extras.log'
    environment = dict(os.environ, HKPSP_ASSETRIPPER=str(args.assetripper.resolve(strict=True)),
                       PYTHONIOENCODING='utf-8')
    selected = [job for job in JOBS if not args.only or job.name in args.only]
    unknown = set(args.only or ()) - {job.name for job in JOBS}
    if unknown:
        raise SystemExit(f'Unknown extra export jobs: {sorted(unknown)}')
    failed = []
    pending = [job for job in selected if not (stamps / f'{job.name}.ok').is_file()]
    # Group session jobs so each set of scenes is loaded once.
    groups = {}
    for job in pending:
        key = (job.kind, job.scenes, job.port) if job.kind == 'session' else (job.kind, job.name)
        groups.setdefault(key, []).append(job)
    for (kind, *rest), jobs in groups.items():
        def finish(job, code):
            if code:
                failed.append(job.name)
            else:
                (stamps / f'{job.name}.ok').write_text('ok\n', encoding='utf-8')
        if kind == 'session':
            levels = scene_levels(args.game_data, jobs[0].scenes)
            try:
                with game_session(args.assetripper, args.game_data, levels,
                                  f'assetripper-extras-{jobs[0].name}.log', port=jobs[0].port) as _client:
                    for job in jobs:
                        finish(job, run_job(job, sys.executable, environment, log))
            except Exception as error:  # A session failure fails only its jobs.
                print(f'Session for {[job.name for job in jobs]} failed: {error}', flush=True)
                failed.extend(job.name for job in jobs if job.name not in failed)
        else:
            for job in jobs:
                finish(job, run_job(job, sys.executable, environment, log))
    if failed:
        print(f'Failed extra exports ({len(failed)}): {", ".join(failed)}. See {log}.', flush=True)
        return 1
    mirror_mantis_scenery()
    print(f'Extra exports ready: {len(selected)} jobs', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
