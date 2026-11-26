"""Reviewed ownership of the active Tutorial_01 package and saved export steps.

Paths are relative to the checkout, except SDK tools. Export directories are
inputs, not targets: the graph never contacts a running original/AssetRipper.
"""
from regen import Rule, local_modules, ordered
from extract_scenes import SCENES as EXPORTED_SCENES
from convert_scenes import ORIGINAL_STAG_STATIONS, CHECKOUT_SCENES, scene_source
from deepnest_scenes import SCENES as DEEPNEST_SCENES
from greenpath_scenes import SCENES as GREENPATH_SCENES
from scene_sources import saved_scenes
from assetripper_client import ROOT
from pathlib import Path
import os
from check_memory_budget import _cpp_constants, source_actor_banks, actor_metadata_file
from scene_particles import particle_source


ACTOR_RULES={'tiktik':'crossroads-enemies-a','gruzzer':'crossroads-enemies-a',
             'vengefly':'crossroads-enemies-a','aspid-hatchling':'crossroads-enemies-a',
             'enemy-b':'enemy-b','enemy-c':'enemy-c','boss':'crossroads-boss-art',
             'false-knight':'false-knight-fullres-art','watcher':'watcher-art',
             'greenpath':'greenpath-enemies','mantis':'mantis-art','elder-baldur':'elder-baldur'}

SCENES = {**EXPORTED_SCENES, **GREENPATH_SCENES, **DEEPNEST_SCENES, **CHECKOUT_SCENES}
from pickup_assets import SCENES as PICKUP_SCENES
from permanent_pickup_assets import SCENES as PERMANENT_SCENES
from tool_paths import TEX3DS as TEX3DS_EXE

COLLECTIBLE_SCENES = PICKUP_SCENES | PERMANENT_SCENES

RESEARCH = '_private/research/'
CONVERTED = '_private/converted/'
ROMFS = CONVERTED + 'hero/romfs'
SCRATCH = CONVERTED + 'regen/'
TEX3DS = TEX3DS_EXE
FFMPEG = '_private/tools/ffmpeg-7.1.1/ffmpeg-7.1.1-full_build-shared/bin/ffmpeg.exe'


def tutorial_scene_inputs():
    """Steam builds export the King's Pass border source; development trees
    import the runtime capture into room-visuals instead."""
    root = '_private/research/tutorial-scene'
    return (root,) if (ROOT / root).is_dir() else ()


def optional_tool(name, *patterns):
    """Bundled private tool folders are cache inputs only where a checkout has
    them; a fresh checkout imports the same pinned packages from HKPSP_PYTHON."""
    root = '_private/tools/' + name
    if not (ROOT / root).is_dir():
        return ()
    return tuple(f'{root}/{pattern}' for pattern in patterns) if patterns else (root,)


def unitypy_inputs():
    # Python -E ignores PYTHONPATH; the actor converter supports an explicit dependency root.
    path = Path(os.environ.get('HK3DS_UNITYPY_PATH', str(ROOT / '_private/tools/boss-python')))
    if not path.is_absolute():
        path = ROOT / path
    if 'HK3DS_UNITYPY_PATH' not in os.environ and not path.exists():
        return ()  # Fresh checkout: the packages come from HKPSP_PYTHON.
    return (path.relative_to(ROOT).as_posix(),) if path.is_relative_to(ROOT) else ()


def command(script, *args):
    return ('{python}', '-E', 'tools/' + script + '.py', *args)


def room_scenes():
    """All rooms with generation rules, including complete saved exports."""
    return sorted(set(SCENES) | set(saved_scenes(ROOT / RESEARCH / "crossroads-scenes")))


def rules(*, actor_scenes=None, scene_filter=None):
    from convert_hud import geo_font_source
    from convert_lighting import LIGHTING_INPUTS
    from convert_room_entities import GAME_DATA, LANDING_INPUTS
    # An explicitly selected package root is always an input; the default
    # bundled folder only where this checkout has it (see optional_tool).
    selected = os.environ.get('HK3DS_BOSS_PACKAGES')
    if selected:
        boss_packages = Path(selected)
        if boss_packages.is_absolute():
            boss_packages = boss_packages.relative_to(ROOT)
        boss_package_inputs = (boss_packages.as_posix(),)
    else:
        boss_package_inputs = optional_tool('boss-python')
    graph = []
    graph.append(Rule('route-tolls', (command('convert_route_tolls'),),
                      tuple('_private/original/Hollow Knight/hollow_knight_Data/' + name
                            for name in ('level80', 'level272', 'resources.assets', 'sharedassets9.assets',
                                         'sharedassets9.assets.resS', 'sharedassets0.assets', 'sharedassets1.assets',
                                         'sharedassets6.assets', 'sharedassets7.assets',
                                         'globalgamemanagers.assets', 'Resources/unity_builtin_extra', 'Managed')) + unitypy_inputs() + (TEX3DS,) +
                      (RESEARCH+'decompiled/all/StringEncrypt.cs',) +
                      tuple(RESEARCH+'crossroads-scenes/Crossroads_49b/terrain/level80-'+str(identity)+'.json'
                            for identity in (558, *range(568, 582))),
                      tuple(ROMFS+'/route-tolls/'+name for name in
                            ('Crossroads_49b.bin', 'Mines_33.bin', 'source.json')) +
                      tuple(path for stem in ('city-toll-actor','mines-toll-actor') for path in
                            (CONVERTED+'route-tolls/'+stem+'-page-0.png',
                             CONVERTED+'route-tolls/'+stem+'-source.json',
                             ROMFS+'/'+stem+'-art.bin',ROMFS+'/'+stem+'-page-0.t3x'))))
    graph.append(Rule('greenpath-tolls', (command('convert_greenpath_tolls'),),
                      tuple('_private/original/Hollow Knight/hollow_knight_Data/' + name
                            for name in ('level158', 'resources.assets', 'sharedassets9.assets',
                                         'sharedassets9.assets.resS', 'sharedassets0.assets', 'sharedassets1.assets',
                                         'sharedassets6.assets', 'sharedassets7.assets',
                                         'globalgamemanagers.assets', 'Resources/unity_builtin_extra', 'Managed')) + unitypy_inputs() + (TEX3DS,) +
                      (RESEARCH+'decompiled/all/StringEncrypt.cs', RESEARCH+'crossroads-scenes/Fungus1_31/terrain/**/*'),
                      (ROMFS+'/route-tolls/Fungus1_31.bin', ROMFS+'/route-tolls/Fungus1_31-source.json',
                       CONVERTED+'greenpath-tolls/greenpath-toll-actor-page-0.png',
                       CONVERTED+'greenpath-tolls/greenpath-toll-actor-source.json',
                       ROMFS+'/greenpath-toll-actor-art.bin', ROMFS+'/greenpath-toll-actor-page-0.t3x')))
    graph.append(Rule('sly-shop', (command('convert_sly_shop'),),
                      tuple('_private/original/Hollow Knight/hollow_knight_Data/' + name
                            for name in ('level17', 'sharedassets17.assets', 'sharedassets17.assets.resS',
                                         'resources.assets', 'sharedassets0.assets', 'sharedassets6.assets',
                                         'sharedassets7.assets', 'sharedassets9.assets', 'sharedassets10.assets',
                                         'globalgamemanagers.assets', 'Resources/unity_builtin_extra', 'Managed')) +
                      unitypy_inputs() +
                      (RESEARCH+'decompiled/all/StringEncrypt.cs', TEX3DS) +
                      tuple(RESEARCH+'sly-shop-icons/'+flag+extension
                            for flag in ('slyShellFrag1','slyShellFrag2','slyVesselFrag1','slySimpleKey','gotCharm_1','gotCharm_4','hasLantern','slyRancidEgg')
                            for extension in ('.json','.png')),
                      ('include/sly_shop_data.hpp', CONVERTED+'sly-shop/source.json', ROMFS+'/sly-shop.bin',
                       CONVERTED+'sly-shop/text-fragment.json',
                       CONVERTED+'sly-shop/icons-source.json', CONVERTED+'sly-shop/sly-shop-icons.bin',
                       CONVERTED+'sly-shop/sly-shop-icons.png', CONVERTED+'sly-shop/sly-shop-icons.t3x',
                       ROMFS+'/sly-shop-icons.bin', ROMFS+'/sly-shop-icons.t3x',
                       CONVERTED+'sly-shop/sly-shop-actor-page-0.png', CONVERTED+'sly-shop/sly-shop-actor-source.json',
                       ROMFS+'/sly-shop-actor-art.bin', ROMFS+'/sly-shop-actor-page-0.t3x')))
    graph.append(Rule('world-quests', (command('world_quest_assets'),),
                      ('_private/original/Hollow Knight/hollow_knight_Data/level20',
                       '_private/original/Hollow Knight/hollow_knight_Data/resources.assets',
                       RESEARCH+'decompiled/all/StringEncrypt.cs',
                       RESEARCH+'tutorial-reminders/prompt-map.json'),
                      (ROMFS+'/world-quests.bin', CONVERTED+'world-quests/source.json')))
    graph.append(Rule('world-quest-art', (command('world_quest_art'),),
                      ('_private/minimum-world-quests/art', '_private/research/area-title', TEX3DS),
                      (CONVERTED+'world-quest-art/**/*',),
                      copies=((CONVERTED+'world-quest-art/romfs/*', ROMFS),)))
    graph.append(Rule('bretta-quest', (command('world_quest_assets', '--quest', 'bretta'),),
                      ('_private/original/Hollow Knight/hollow_knight_Data/level187',
                       '_private/original/Hollow Knight/hollow_knight_Data/resources.assets',
                       RESEARCH+'decompiled/all/StringEncrypt.cs',
                       RESEARCH+'tutorial-reminders/prompt-map.json'),
                      (ROMFS+'/bretta-quest.bin', CONVERTED+'world-quests/bretta-source.json')))
    graph.append(Rule('bretta-quest-art', (command('world_quest_art', '--quest', 'bretta'),),
                      ('_private/minimum-world-quests/bretta-art', TEX3DS),
                      (CONVERTED+'bretta-quest-art/**/*',),
                      copies=((CONVERTED+'bretta-quest-art/romfs/*', ROMFS),)))
    room_budget=str(_cpp_constants(Path(__file__).resolve().parents[1]/'include/render_memory.hpp')['roomTextureBudget'])
    catalog_scenes = room_scenes()
    if scene_filter is not None:
        missing = set(scene_filter) - set(catalog_scenes)
        if missing:
            raise ValueError('Route scenes lack source exports: ' + ', '.join(sorted(missing)))
    scenes = catalog_scenes if scene_filter is None else [scene for scene in catalog_scenes if scene in scene_filter]
    for scene in scenes:
        target = ROMFS + '/scenes/' + scene + '/'
        scale = ('--texture-scale', '1') if scene.startswith(('Fungus2_', 'Fungus3_')) else ()
        converter = 'convert_greenpath_scenes' if scene in GREENPATH_SCENES else 'convert_scenes'
        room_outputs = ('scene*.bin', 'activation.bin', 'battle-gates.bin', 'rest.bin', 'stag.bin', 'doors.bin', 'breakables.bin',
                        'greenpath-enemies.bin', 'tutorial*.bin', 'terrain-motion.bin', 'terrain-walls.bin', 'terrain-levers.bin', 'room-*')
        room_outputs += ('gate-inputs.bin', 'world-obstacles.bin')
        if scene == 'Crossroads_03':
            room_outputs += ('world-switch*',)
        if scene == 'Crossroads_47':
            room_outputs += ('stag-station-art*',)
        if scene == 'Fungus2_21':
            room_outputs += ('city-crest*', 'city-gate.bin', 'city-gate-*.wav')
        if scene == 'RestingGrounds_09':
            room_outputs += ('stag-lever*',)
        if scene == 'Town':
            room_outputs += ('town-lift*', 'jiji-*.bin')
        if scene == 'Crossroads_49b':
            room_outputs += ('city-toll*',)
        if scene in ('Crossroads_49b','Crossroads_49'):
            room_outputs += ('city-lift*','city-transport*')
        if scene == 'Ruins1_05b':
            room_outputs += ('waterways-machine*', 'waterways-art*')
        if scene in ('Crossroads_10', 'Waterways_05'):
            room_outputs += ('boss-room.bin',)
        if scene == 'Fungus2_15':
            room_outputs += ('ambient-particles.bin',)
        scene_args = ('--scene', scene) + scale
        respawn_sources = ('_private/minimum-obstacles/respawn-markers/' + scene
                           if scene in ('Crossroads_11_alt', 'Fungus1_01', 'Fungus1_02', 'Fungus1_03', 'Fungus1_04', 'Fungus1_06', 'Fungus1_07', 'Fungus1_17') else None)
        if scene == 'Crossroads_16':
            respawn_sources = '_private/converted-psp/route-scenes-source/Crossroads_16-markers'
        if respawn_sources:
            scene_args += ('--obstacle-sources', respawn_sources)
        if scene in DEEPNEST_SCENES or scene == 'Fungus3_archive_02':
            scene_args += ('--full-scale',)
        if converter == 'convert_scenes':
            scene_args += ('--source', scene_source(scene).as_posix(), '--supported-scenes')
        particles = particle_source(ROOT / scene_source(scene) / 'visuals')
        # Scoped graphs still declare resident actor inputs for access auditing.
        if actor_scenes is None or scene in actor_scenes:
            declared_banks=source_actor_banks(scene,ROOT/scene_source(scene))
            if scene == 'Fungus1_04':
                declared_banks.add('greenpath')
            banks=declared_banks
        else:
            declared_banks=set(ACTOR_RULES)
            banks=set()
        # The particle reader resolves the shared research junction. Declare its
        # worktree alias so fingerprints and access-audit names agree.
        particle_inputs = ('_private/fungus-particles/' + particles.name,
                           '_private/research/ambient-particles/mesh-10209.json') if particles is not None else ()
        stage = CONVERTED + 'scene-terrain/' + scene
        terrain_rule = 'terrain-scene-' + scene
        scene_inputs = (scene_source(scene).as_posix(),)
        if scene == 'Fungus1_04':
            scene_inputs += (RESEARCH+'hornet-protector/scene.json', 'tools/hornet_room.py')
            # The additive scene's resident visual export references the copied
            # Greenpath particle source directly, even though level132 has no
            # scene-manager override entry.
            # Declare the required tree even before extraction. Fingerprinting
            # tracks its contents recursively and rejects a missing directory.
            scene_inputs += ('_private/fungus-particles/zone6',)
        if scene == 'Fungus2_21':
            scene_inputs += unitypy_inputs() + optional_tool('audio-python') + (
                '_private/research/decompiled/all/StringEncrypt.cs',)
            scene_inputs += ('_private/original/Hollow Knight/hollow_knight_Data/level186',
                             '_private/original/Hollow Knight/hollow_knight_Data/resources.assets',
                             '_private/original/Hollow Knight/hollow_knight_Data/sharedassets6.assets',
                             '_private/original/Hollow Knight/hollow_knight_Data/sharedassets6.resource',
                             '_private/original/Hollow Knight/hollow_knight_Data/sharedassets7.assets',
                             '_private/original/Hollow Knight/hollow_knight_Data/sharedassets7.resource',
                             '_private/original/Hollow Knight/hollow_knight_Data/resources.resource')
        if scene == 'Waterways_05':
            scene_inputs += ((scene_source(scene).parent / 'Waterways_05_boss').as_posix(),)
        graph.append(Rule(terrain_rule,
                          (command('convert_room_terrain', '--source', scene_source(scene).as_posix() + '/terrain',
                                   '--output', stage + '/terrain', '--romfs', stage + '/romfs'),),
                          (scene_source(scene).as_posix(),), (stage + '/**/*',)))
        scene_args += ('--terrain-stage', stage)
        scene_commands = (command(converter, *scene_args),)
        scene_inputs += (stage, TEX3DS, 'include/render_memory.hpp', 'include/graphics_profile.hpp')
        if respawn_sources:
            scene_inputs += (respawn_sources,)
        if scene.startswith('Fungus1_'):
            scene_inputs += tuple(ROMFS + '/' + name + '.bin' for name in (
                'wind-green_path_atmos_loop', 'music-greenpath-bass', 'music-greenpath-main'))
        scene_inputs += ( 'tools/breakable_hierarchy.py',
                         'tools/room_breakable_effects.py', 'tools/room_breakable_fling.py',
                         'tools/room_breakable_audio.py', 'tools/convert_decor_effects.py')
        if scene == 'Waterways_13':
            scene_inputs += optional_tool('boss-python') + (
                             '_private/original/Hollow Knight/hollow_knight_Data/sharedassets33.assets',
                             '_private/original/Hollow Knight/hollow_knight_Data/sharedassets33.assets.resS',
                             '_private/original/Hollow Knight/hollow_knight_Data/Managed/TeamCherry.TK2D.dll',
                             '_private/original/Hollow Knight/hollow_knight_Data/Managed/netstandard.dll',
                             '_private/original/Hollow Knight/hollow_knight_Data/Managed/mscorlib.dll',
                             '_private/original/Hollow Knight/hollow_knight_Data/Managed/UnityEngine.CoreModule.dll',
                             '_private/original/Hollow Knight/hollow_knight_Data/level362')
        if scene == 'Ruins2_Watcher_Room':
            scene_inputs += (RESEARCH + 'route-watcher-animation/**/*',)
        if scene == 'Fungus2_15':
            scene_inputs += optional_tool('boss-python', '**/*.py', '**/*.json', '**/*.tpk',
                                          '**/entry_points.txt')
            # Mantis tk2d sprites resolve original atlas PPtrs and script types.
            original = '_private/original/Hollow Knight/hollow_knight_Data/'
            scene_inputs += tuple(original + name for name in (
                'level179', 'resources.assets', 'globalgamemanagers.assets',
                'Resources/unity_builtin_extra', 'Managed/*.dll',
                'sharedassets168.assets.resS'))
            scene_inputs += tuple(original + f'sharedassets{number}.assets'
                                  for number in (0, 1, 6, 7, 9, 19, 23, 27, 31,
                                                 37, 39, 50, 56, 65, 88, 134, 167, 168))
        if scene == 'Cutscene_Boss_Door':
            scene_inputs += ('_private/minimum-bosses/monomon-mask', '_private/minimum-bosses/uumuu/level403-source.json')
        scene_after = ()
        if scene == 'Dream_Guardian_Monomon':
            scene_after = ('monomon-rules', 'monomon-guardian-art')
        elif scene == 'Cutscene_Boss_Door':
            scene_after = ('monomon-rules',)
        elif scene == 'Fungus3_47':
            scene_after = ('archive-shortcut',)
        elif scene == 'Fungus3_archive_02':
            scene_after = ('monomon-rules', 'monomon-archive-art', 'monomon-presentation',
                           'uumuu-audio', 'uumuu-effects-art', 'uumuu-gate-art', 'uumuu-room',
                           'uumuu-gates', 'uumuu-wake', 'uumuu-combat', 'uumuu-quirrel',
                           'uumuu-quirrel-watch', 'uumuu-attacks', 'uumuu-motion',
                           'uumuu-multizaps', 'uumuu-zap', 'uumuu-death', 'uumuu-body-art',
                           'uumuu-quirrel-art')
        if scene == 'Crossroads_03':
            scene_commands += (command('convert_world_switch_visuals', '--source', '_private/minimum-obstacles/switch-art', '--output', target, '--scene-source', RESEARCH + 'crossroads-scenes/Crossroads_03/visuals', '--tex3ds', TEX3DS),)
            scene_inputs += ('_private/minimum-obstacles/switch-art',)
        if scene == 'Crossroads_06':
            scene_commands += (command('convert_shaman_pillar','--output',target),)
            scene_inputs += ('_private/minimum-obstacles/switch-art', '_private/minimum-obstacles/shaman-pillar-feedback', RESEARCH+'hero-light/level6-12820.json')
            room_outputs += ('shaman-pillar*',)
        if scene == 'Fungus2_04':
            scene_commands += (command('convert_mantis_gates','--output',target),)
            scene_inputs += ('_private/minimum-obstacles/mantis-gate-art',)
            room_outputs += ('mantis-gate*',)
        if scene == 'Crossroads_47':
            scene_commands += (command('convert_stag_station_art', '--art', '_private/minimum-obstacles/stag-station-art',
                                       '--output', target, '--scene-source', RESEARCH + 'crossroads-scenes/Crossroads_47/visuals',
                                       '--tex3ds', TEX3DS),)
            scene_inputs += ('_private/minimum-obstacles/stag-station-art',
                             RESEARCH + 'crossroads-scenes/Crossroads_47/visuals')
        if scene == 'RestingGrounds_09':
            scene_commands += (command('convert_stag_lever', '--output', target),)
            scene_inputs += ('_private/minimum-obstacles/resting-station-art',
                             '_private/minimum-obstacles/resting-station-feedback',
                             RESEARCH+'hero-light/level6-12820.json')
            room_outputs += ('stag-bell-rise.wav',)
        if scene == 'Town':
            scene_commands += (command('convert_town_lift', '--output', target), command('convert_jiji_door', '--output', target))
            scene_inputs += ('_private/minimum-obstacles/town-art','_private/minimum-obstacles/town-lever-feedback',RESEARCH+'tutorial-reminders/EN_Prompts-decrypted.xml','_private/original/Hollow Knight/hollow_knight_Data/level7')
        if scene == 'Fungus2_21':
            scene_commands += (command('convert_city_crest', '--output', target),)
            scene_commands += (command('convert_city_crest_audio', '--output', target), command('convert_city_crest_effects', '--output', target))
            scene_inputs += ('_private/minimum-obstacles/city-crest-audio', '_private/minimum-obstacles/city-crest-effects', '_private/minimum-obstacles/city-crest-slam-art', '_private/minimum-obstacles/city-crest-rocks', RESEARCH+'hero-light/level6-12820.json')
            scene_inputs += ('_private/minimum-obstacles/city-crest-source', RESEARCH+'tutorial-reminders/EN_Prompts-decrypted.xml', RESEARCH+'decompiled/all/GlobalEnums/MapZone.cs')
        if scene == 'Ruins1_05b':
            scene_commands += (command('convert_waterways_machine', '--output', target), command('convert_waterways_art', '--output', target))
            scene_inputs += ('_private/minimum-obstacles/waterways-art',RESEARCH+'tutorial-reminders/EN_Prompts-decrypted.xml',
                             RESEARCH+'crossroads-scenes/Ruins1_05b/visuals')
        if scene == 'Crossroads_49b':
            scene_commands += (command('convert_city_toll','--output',target),)
            scene_inputs += tuple(RESEARCH+'crossroads-scenes/Crossroads_49b/terrain/level80-'+str(n)+'.json' for n in (29,294,559,819))
            scene_inputs += (RESEARCH+'lore-tablets/resources.assets-21856.json',RESEARCH+'lore-tablets/resources.assets-22339.json',RESEARCH+'tutorial-reminders/EN_Prompts-decrypted.xml',RESEARCH+'stag-stations/EN_Prices-decrypted.xml')
        if scene in ('Crossroads_49b','Crossroads_49'):
            source=RESEARCH+'crossroads-scenes/'+scene
            scene_commands += (command('convert_city_lift','--source',source+'/terrain','--output',target), command('convert_city_lift_art','--source',source+'/visuals','--output',target), command('convert_city_transport','--source',source+'/terrain','--output',target))
            scene_inputs += (source+'/terrain',source+'/visuals','_private/minimum-obstacles/city-lift-art')
        graph.append(Rule('scene-' + scene, scene_commands,
                          tuple(dict.fromkeys(scene_inputs + particle_inputs +
                              ((RESEARCH+'ambient-particles/mesh-10209.json',) if scene == 'Fungus2_15' else ()) +
                              ((RESEARCH + 'stag-stations/EN_Prices-decrypted.xml',) if scene in {entry[0] for entry in ORIGINAL_STAG_STATIONS} else ()))) +
                          tuple(ROMFS+'/'+pattern for bank in sorted(declared_banks)
                                for pattern in (f'{bank}-page-*.t3x',actor_metadata_file(bank))),
                          # Match direct temporary files even after atomic rename.
                          (CONVERTED + 'scenes/' + scene + '/*',
                           CONVERTED + 'scenes/' + scene + '/**/*') +
                          # Collectibles have a separate owner, even inside a room.
                          tuple(target + pattern for pattern in room_outputs + ('acid.bin',)),
                          after=tuple(sorted({ACTOR_RULES[bank] for bank in banks} | ({'mantis-art'} if scene == 'Fungus2_15' else set()))) + (terrain_rule,) + scene_after +
                                (('stag',) if scene in {entry[0] for entry in ORIGINAL_STAG_STATIONS} else ()) +
                                (('broken-vessel-combat', 'broken-vessel-art') if scene == 'Abyss_19' else ()) +
                                (('dung-defender-art',) if scene == 'Waterways_05' else ()) +
                                (('decor-effects',) if scene.startswith('Fungus2_') else ()) +
                                (('bretta-quest', 'bretta-quest-art') if scene == 'Fungus2_23' else ()) +
                                (('world-quests', 'world-quest-art') if scene == 'Room_ruinhouse' else ()) +
                                (('sly-shop',) if scene == 'Room_shop' else ()) +
                                (('route-tolls',) if scene in ('Crossroads_49b', 'Mines_33') else ()) +
                                (('greenpath-tolls',) if scene == 'Fungus1_31' else ()) +
                                (('false-knight-fullres-art',) if scene == 'Crossroads_10' else ()) +
                                (('wind-audio', 'music-audio') if scene.startswith('Fungus1_') else ()),
                          gate_scenes=tuple(dict.fromkeys((*SCENES, 'Tutorial_01', *catalog_scenes))) if converter == 'convert_scenes' else (),
                          gate_source=scene_source(scene).as_posix() if converter == 'convert_scenes' else '',
                          optional_inputs=(RESEARCH + 'route-watcher-animation/**/*',)
                                          if scene == 'Ruins2_Watcher_Room' else (),
                          # Old static files are temporary migration outputs removed during replacement.
                          temporary_outputs=((target + 'static-*',) if scene in ('Town', 'Crossroads_07') else ()) +
                                            (CONVERTED + 'scenes/' + scene + '/.scene-*/*',) +
                                            tuple(ROMFS + '/.scene-*/' + scene + '/' + pattern
                                                  for pattern in room_outputs + ('acid.bin',))))
        if scene in ('Town', 'Crossroads_07'):
            static_decor = CONVERTED + 'static-decor/' + scene
            resident = CONVERTED + 'scenes/' + scene + '/resident'
            static_target = ROMFS + '/static-scenes/' + scene + '/'
            graph.append(Rule('room-' + scene + '-static-decor', (
                command('static_decor_bake', '--bank', target + 'room-resident-visual.bin',
                        '--page-directory', resident, '--page-prefix', 'room-resident-page-',
                        '--source', scene_source(scene).as_posix() + '/visuals', '--screen-scale', '2',
                        '--output', static_decor),),
                (resident + '/*', target + 'room-resident-*', scene_source(scene).as_posix(),
                 TEX3DS, 'include/render_memory.hpp'), (static_decor + '/*',),
                after=('scene-' + scene,),
                copies=((static_decor + '/static-*.bin', static_target),
                        (static_decor + '/static-room-page-*.t3x', static_target))))
    graph.append(Rule('scene-collectibles', (command('convert_scenes', '--collectibles-only'),),
                      tuple('_private/original/Hollow Knight/hollow_knight_Data/level' + str(n)
                            for n in COLLECTIBLE_SCENES.values()),
                      (CONVERTED + 'collectibles/**/*', ROMFS + '/collectibles.bin') +
                      tuple(ROMFS + '/scenes/' + scene + '/collectibles.bin'
                            for scene in COLLECTIBLE_SCENES if scene != 'Tutorial_01'),
                      # Scene conversion replaces the entire room directory, including this file.
                      after=tuple('scene-' + scene for scene in scenes if scene in COLLECTIBLE_SCENES)))

    def pack(name, script, exports, output=None, extra=(), headers=(), after=(), texture=True):
        stage = SCRATCH + name
        graph.append(Rule(name, (command(script, '--romfs', stage),),
                          tuple(RESEARCH + e for e in exports) + ((TEX3DS,) if texture else ()) + tuple(extra),
                          ((CONVERTED + (output or name) + '/**/*',) + tuple(headers) + (stage + '/*',)),
                          after=after, copies=((stage + '/*', ROMFS),), scratch=(stage,)))

    graph.append(Rule('hero', (command('convert_hero'),),
                      tuple(RESEARCH + e for e in ('assets', 'hero-effects', 'focus-effects', 'hero-death',
                                                  'hero-damage-flash', 'hero-damage-flings',
                                                  'hero-light/level6-12820.json')) + (TEX3DS,),
                      (CONVERTED + 'hero/*', ROMFS + '/hero.bin', ROMFS + '/hero-effects.bin', ROMFS + '/focus-effects.bin',
                       ROMFS + '/nail.bin', ROMFS + '/page-*.t3x')))
    graph.append(Rule('tutorial', (command('convert_tutorial'),),
                      (RESEARCH + 'tutorial',
                       '_private/original/Hollow Knight/hollow_knight_Data/level6',
                       '_private/original/Hollow Knight/hollow_knight_Data/Managed/Assembly-CSharp.dll'),
                      (CONVERTED + 'tutorial/manifest.json', CONVERTED + 'tutorial/tutorial-room.bin')))
    pack('room-terrain', 'convert_room_terrain', ('terrain', 'room-visuals'), texture=False)
    # This converter has no --romfs flag; it owns only its door bank.
    graph.append(Rule('breakable', (command('convert_breakable'),), (RESEARCH + 'terrain', RESEARCH + 'room-visuals'),
                      (CONVERTED + 'door/*', ROMFS + '/door.bin')))
    graph.append(Rule('room-rules', (command('convert_room_rules'),),
                      tuple(RESEARCH + e for e in ('tutorial', 'terrain', 'room-visuals', 'fury-mask',
                                                  'assets/c1-20600.json', 'assets/c1-20602.json',
                                                  'enemy/resources.assets-13852.json')) +
                      (CONVERTED + 'tutorial/manifest.json', CONVERTED + 'room-terrain/manifest.json'),
                      (ROMFS + '/room-rules.bin', CONVERTED + 'tutorial/rules-manifest.json'),
                      after=('tutorial', 'room-terrain')))
    for name, script, exports, files in (
        ('room-entities', 'convert_room_entities',
         ('tutorial', 'enemy', 'room-entities', 'assets/Physics2DSettings.json', 'assets/TimeManager.json',
          'decompiled/all'),
         ('room-entities.bin', 'room-entities-manifest.json')),
        ('geo', 'convert_geo', ('geo', 'tutorial', 'enemy', 'assets/Physics2DSettings.json',
          *(f'decompiled/all/{name}*.cs' for name in
            ('HealthManager', 'GeoControl', 'FlingUtils', 'ObjectBounce', 'EnemyDeathEffects'))),
         ('geo.bin', 'geo-manifest.json')),
        ('great-door', 'convert_great_door', ('great-door', 'tutorial', 'decompiled/all'),
         ('great-door.bin', 'great-door-manifest.json')),
    ):
        original = ()
        if name == 'room-entities':
            original = tuple(GAME_DATA + '/' + path for path in LANDING_INPUTS)
        graph.append(Rule(name, (command(script),), tuple(RESEARCH + e for e in exports) + original,
                          tuple(CONVERTED + 'tutorial/' + f for f in files),
                          copies=((CONVERTED + 'tutorial/' + files[0], ROMFS),)))
    pack('crawler', 'convert_crawler', ('enemy',))
    pack('enemy-b', 'convert_enemy_b', ('crossroads-enemies-b',))
    pack('buzzer', 'convert_buzzer', ('buzzer', 'tutorial'))
    graph.append(Rule('greenpath-enemies', (command('convert_greenpath_enemies', '--no-romfs-copy'),),
                      (CONVERTED+'greenpath-enemies-source', TEX3DS),
                      (CONVERTED+'greenpath-enemies/**/*', 'include/greenpath_enemy_data.hpp'),
                      copies=((CONVERTED+'greenpath-enemies/romfs/*', ROMFS),)))
    pack('enemy-c', 'convert_enemy_c', ('crossroads-enemies-c',), extra=(CONVERTED+'battle-enemies-source',),
         headers=('include/enemy_c_data.hpp',))
    graph.append(Rule('elder-baldur', (command('convert_elder_baldur'),),
                      # Native-library references loaded by UnityPy; audited actual reads.
                      tuple('_private/original/Hollow Knight/hollow_knight_Data/'+name for name in (
                          'Resources/unity_builtin_extra',
                          'globalgamemanagers.assets',
                          'level50',
                          'resources.assets',
                          'sharedassets0.assets',
                          'sharedassets1.assets',
                          'sharedassets13.assets',
                          'sharedassets19.assets',
                          'sharedassets21.assets',
                          'sharedassets23.assets',
                          'sharedassets27.assets',
                          'sharedassets28.assets',
                          'sharedassets30.assets',
                          'sharedassets31.assets',
                          'sharedassets32.assets',
                          'sharedassets32.assets.resS',
                          'sharedassets33.assets',
                          'sharedassets34.assets',
                          'sharedassets37.assets',
                          'sharedassets42.assets',
                          'sharedassets50.assets',
                          'sharedassets50.assets.resS',
                          'sharedassets6.assets',
                          'sharedassets7.assets',
                          'sharedassets9.assets',
                          'Managed')) + (TEX3DS,) + unitypy_inputs(),
                      (CONVERTED+'elder-baldur/**/*',),
                      copies=((CONVERTED+'elder-baldur/romfs/*', ROMFS),)))
    graph.append(Rule('enemy-rewards', (command('convert_enemy_rewards'),),
                      tuple(RESEARCH + path for path in (
                          'enemy/*.json', 'tutorial/inventory.json', 'tutorial/level6-*.json',
                          'crossroads-enemies-a/*.json', 'crossroads-enemies-b/*.json',
                          'crossroads-enemies-c/*.json',
                          'crossroads-scenes/Crossroads_ShamanTemple/terrain/terrain-map.json',
                          'crossroads-scenes/Crossroads_ShamanTemple/terrain/level76-14804.json'))
                      + (CONVERTED+'greenpath-enemies-source/source-components.json',),
                      (CONVERTED+'enemy-rewards/manifest.json', 'include/enemy_reward_data.hpp')))
    pack('crossroads-enemies-a', 'convert_crossroads_enemies_a',
         ('crossroads-enemies-a', 'buzzer', 'enemy', 'assets/Physics2DSettings.json'),
         extra=(CONVERTED+'tutorial/room-entities.bin',), after=('room-entities',))

    graph.append(Rule('dung-defender-art',
                      (command('convert_dung_defender', '--snapshot', RESEARCH + 'dung-defender'),),
                      (RESEARCH + 'dung-defender', TEX3DS),
                      (CONVERTED + 'dung-defender/**/*',),
                      copies=((CONVERTED + 'dung-defender/romfs/*', ROMFS),)))
    graph.append(Rule('crossroads-boss-art',
                      (command('convert_crossroads_bosses', '--snapshot', RESEARCH + 'crossroads-bosses'),),
                      (RESEARCH + 'crossroads-bosses', TEX3DS),
                      (CONVERTED + 'crossroads-bosses/**/*',),
                      copies=((CONVERTED + 'crossroads-bosses/romfs/*', ROMFS),)))
    graph.append(Rule('watcher-combat', (command('convert_watcher_knights'),),
                      ('_private/minimum-bosses/watcher-boss-source.json',
                       '_private/minimum-bosses/watcher-boss-transforms.json','_private/minimum-bosses/physics.json'),
                      ('include/watcher_knights_data.hpp',)))
    graph.append(Rule('uumuu-audio', (command('convert_uumuu_audio'),),
                      ('_private/minimum-bosses/uumuu-audio',),
                      (CONVERTED+'uumuu-audio/**/*',),
                      copies=((CONVERTED+'uumuu-audio/*.wav', ROMFS),)))
    graph.append(Rule('uumuu-effects-art', (command('convert_uumuu_effects_art'),),
                      ('_private/minimum-bosses/uumuu-effects-art', '_private/research/false-knight-effects',
                       '_private/minimum-bosses/uumuu/corpse-source.json','tools/uumuu_blood_art.py',TEX3DS),
                      (CONVERTED+'uumuu-effects/**/*','include/uumuu_blood_data.hpp'),
                      copies=((CONVERTED+'uumuu-effects/romfs/*', ROMFS),)))
    graph.append(Rule('uumuu-gate-art', (command('convert_uumuu_gate_art'),),
                      ('_private/minimum-bosses/uumuu-gate-art', TEX3DS),
                      (CONVERTED+'uumuu-gate/**/*',),
                      copies=((CONVERTED+'uumuu-gate/romfs/*', ROMFS),)))
    graph.append(Rule('monomon-rules', (command('convert_monomon'),command('convert_monomon_mask')),
                      ('_private/minimum-bosses/uumuu/level402-source.json',
                       '_private/minimum-bosses/uumuu/level228-source.json',
                       '_private/minimum-bosses/uumuu/level403-source.json',
                       '_private/minimum-bosses/monomon-mask/animations.json'),
                      ('include/monomon_data.hpp','include/monomon_admission_data.hpp','include/monomon_mask_data.hpp')))
    graph.append(Rule('monomon-presentation', (command('convert_monomon_presentation'),),
                      ('_private/minimum-bosses/monomon-presentation',),
                      (CONVERTED+'monomon-presentation/**/*','include/monomon_presentation_data.hpp'),
                      copies=((CONVERTED+'monomon-presentation/mask-shatter.bin',ROMFS),),
                      temporary_outputs=(CONVERTED+'monomon-presentation/mask-shatter.rgb',)))
    for stem, arguments in (('monomon-guardian', ()), ('monomon-archive', ('--archive',))):
        graph.append(Rule(stem+'-art', (command('convert_monomon_art', *arguments),),
                          ('_private/minimum-bosses/'+stem+'-art', TEX3DS),
                          (CONVERTED+stem+'/**/*',),
                          copies=((CONVERTED+stem+'/romfs/*', ROMFS),)))
    graph.append(Rule('archive-shortcut', (command('convert_archive_shortcut'),),
                      ('_private/minimum-bosses/uumuu/level223-source.json',),
                      ('include/archive_shortcut_data.hpp',)))
    graph.append(Rule('uumuu-room', (command('convert_uumuu_room'),),
                      ('_private/minimum-bosses/uumuu/level228-source.json',
                       '_private/minimum-bosses/uumuu/level229-source.json',
                       '_private/minimum-bosses/uumuu/level229-objects.json'),
                      ('include/uumuu_room_data.hpp',)))
    graph.append(Rule('uumuu-gates', (command('convert_uumuu_gates'),),
                      ('_private/minimum-bosses/uumuu/level229-source.json',
                       '_private/minimum-bosses/uumuu/level229-objects.json'),
                      ('include/uumuu_gates_data.hpp',)))
    graph.append(Rule('uumuu-wake', (command('convert_uumuu_wake'),),
                      ('_private/minimum-bosses/uumuu/level229-source.json',),
                      ('include/uumuu_wake_data.hpp',)))
    graph.append(Rule('uumuu-combat', (command('convert_uumuu'),),
                      ('_private/minimum-bosses/uumuu/level229-source.json',),
                      ('include/uumuu_data.hpp',)))
    graph.append(Rule('uumuu-quirrel', (command('convert_uumuu_quirrel'),),
                      ('_private/minimum-bosses/uumuu/level229-source.json',),
                      ('include/uumuu_quirrel_data.hpp',)))
    graph.append(Rule('uumuu-quirrel-watch', (command('convert_uumuu_quirrel_watch'),),
                      ('_private/minimum-bosses/uumuu/level229-source.json',),
                      ('include/uumuu_quirrel_watch_data.hpp',)))
    graph.append(Rule('uumuu-attacks', (command('convert_uumuu_attacks'),),
                      ('_private/minimum-bosses/uumuu/level229-source.json',),
                      ('include/uumuu_attack_data.hpp',)))
    graph.append(Rule('uumuu-motion', (command('convert_uumuu_motion'),),
                      ('_private/minimum-bosses/uumuu/level229-source.json',),
                      ('include/uumuu_motion_data.hpp',)))
    graph.append(Rule('uumuu-multizaps', (command('convert_uumuu_multizaps'),),
                      ('_private/minimum-bosses/uumuu/level228-source.json',
                       '_private/research/decompiled/all/MegaJellyZap.cs'),
                      ('include/uumuu_multizap_data.hpp',)))
    graph.append(Rule('uumuu-zap', (command('convert_uumuu_zap'),),
                      ('_private/minimum-bosses/uumuu/pooled-zap-source.json',
                       '_private/research/decompiled/all/MegaJellyZap.cs'),
                      ('include/uumuu_zap_data.hpp',)))
    graph.append(Rule('uumuu-death', (command('convert_uumuu_death'),),
                      ('_private/minimum-bosses/uumuu/corpse-source.json',
                       '_private/research/decompiled/all/GGCheckIfBossScene.cs',
                       '_private/research/decompiled/all/HutongGames.PlayMaker.Actions/TransitionToAudioSnapshot.cs'),
                      ('include/uumuu_death_data.hpp',)))
    graph.append(Rule('broken-vessel-combat', (command('convert_broken_vessel'),),
                      ('_private/minimum-bosses/broken-vessel/level343-source.json',
                       RESEARCH+'assets/Physics2DSettings.json'),
                      ('include/broken_vessel_data.hpp',)))
    graph.append(Rule('broken-vessel-art', (command('convert_broken_vessel_art'),),
                      ('_private/minimum-bosses/broken-vessel/art', TEX3DS),
                      (CONVERTED+'broken-vessel/**/*',),
                      copies=((CONVERTED+'broken-vessel/romfs/*', ROMFS),)))
    graph.append(Rule('watcher-art', (command('convert_watcher_art'),),
                      ('_private/minimum-bosses/art', TEX3DS),
                      (CONVERTED+'watcher-knights/**/*',),
                      copies=((CONVERTED+'watcher-knights/romfs/*', ROMFS),)))
    graph.append(Rule('mantis-art', (command('convert_mantis_lords'),),
                      ('tools/mantis_intro_art.py', 'tools/convert_area_title.py',
                       *boss_package_inputs,
                       '_private/research/area-title/**/*',
                       '_private/original/Hollow Knight/hollow_knight_Data/globalgamemanagers.assets',
                       '_private/original/Hollow Knight/hollow_knight_Data/resources.assets',
                       '_private/original/Hollow Knight/hollow_knight_Data/resources.assets.resS',
                       *('_private/original/Hollow Knight/hollow_knight_Data/sharedassets'+str(n)+'.assets'
                         for n in (0, 1, 6, 7, 9, 13, 19, 23, 27, 30, 31, 34, 37, 39, 40, 50, 56, 65, 88, 134, 167, 168, 179)),
                       '_private/original/Hollow Knight/hollow_knight_Data/sharedassets168.assets.resS',
                       '_private/original/Hollow Knight/hollow_knight_Data/Resources/unity_builtin_extra',
                       '_private/original/Hollow Knight/hollow_knight_Data/level180',
                       '_private/original/Hollow Knight/hollow_knight_Data/sharedassets180*',
                       '_private/original/Hollow Knight/hollow_knight_Data/Managed/*.dll',
                       'tools/convert_crossroads_bosses.py', 'tools/atlas_packing.py', 'tools/watcher_unity.py', TEX3DS),
                      (CONVERTED+'mantis-lords/**/*',),
                      copies=((CONVERTED+'mantis-lords/romfs/*', ROMFS),)))
    graph.append(Rule('uumuu-body-art', (command('convert_uumuu_body_art'),),
                      ('_private/minimum-bosses/uumuu/level229-source.json',
                       '_private/minimum-bosses/uumuu-body-art', 'tools/uumuu_art_layout.py', TEX3DS),
                      (CONVERTED+'uumuu-body/**/*',),
                      copies=((CONVERTED+'uumuu-body/romfs/*', ROMFS),)))
    graph.append(Rule('uumuu-quirrel-art', (command('convert_uumuu_quirrel_art'),),
                      ('_private/minimum-bosses/uumuu/level229-source.json',
                       '_private/minimum-bosses/quirrel-art', 'tools/uumuu_art_layout.py', TEX3DS),
                      (CONVERTED+'uumuu-quirrel/**/*',),
                      copies=((CONVERTED+'uumuu-quirrel/romfs/*', ROMFS),)))
    graph.append(Rule('false-knight-fullres-art',
                      (command('convert_crossroads_bosses', '--false-knight-only', '--scale', '1',
                               '--snapshot', '_private/false-knight-source-art/fullres',
                               '--effects', RESEARCH + 'false-knight-effects',
                               '--output', CONVERTED + 'false-knight'),),
                      ('_private/false-knight-source-art/fullres', RESEARCH + 'false-knight-effects', TEX3DS),
                      (CONVERTED + 'false-knight/**/*',),
                      copies=((CONVERTED + 'false-knight/romfs/*', ROMFS),)))
    pack('great-door-visuals', 'convert_great_door_visuals',
         ('great-door-visuals', 'great-door/level6-11923.json', 'great-door/level6-1806.json', 'room-visuals'))
    pack('geo-visuals', 'convert_geo_visuals', ('geo-visuals', 'assets/Physics2DSettings.json'),
         extra=(CONVERTED + 'geo-chip-source',))
    pack('fury-chest', 'convert_fury_chest',
         ('fury-chest', 'nail-interactions', 'assets/Physics2DSettings.json', 'fury-presentation-source-20261022',
          'fury-reveals-source-20261023', 'tutorial-reminders/EN_Prompts-decrypted.xml'),
         extra=('include/door_prompt_glyphs.hpp',),
         headers=('include/fury_tutorial_data.hpp',))
    pack('shade', 'convert_shade', ('hollow-shade',))
    pack('ability-pickups', 'convert_ability_pickups', ('ability-pickups',),
         extra=('_private/minimum-abilities/spell-source/art',))
    pack('conditional-events', 'convert_conditional_events',
         ('conditional-events', 'lore-tablets', 'tutorial-reminders', 'hero-light/level6-12820.json'),
         headers=('include/mushroom_dialogue_data.hpp',))
    pack('map', 'convert_map', ('bottom-map', 'tutorial-reminders'), headers=('include/map_data.hpp',))
    pack('stag', 'convert_stag', ('stag-stations',), headers=('include/stag_dialogue_data.hpp',))
    pack('cornifer', 'convert_cornifer', ('cornifer',), output='cornifer-original',
         headers=('include/cornifer_data.hpp',))
    pack('title-menu', 'convert_title_menu', ('title-menu',),
         headers=('include/title_font_data.hpp',))
    pack('inventory', 'convert_inventory', ('inventory',), extra=(RESEARCH+'title-menu/title-source.png',))
    pack('hud', 'convert_hud',
         ('hud', 'hud-feedback', 'hud-layout', 'hud-liquid', geo_font_source(ROOT).name,
          'tutorial-reminders', 'opening-credits', 'lore-tablets', 'tablet-glow', 'area-title', 'hud-limiter',
          'hero-light/level6-12820.json'),
         extra=('tools/hud_data.hpp.in', 'include/top_screen_ui_layout.hpp'), headers=tuple('include/' + f for f in
         ('hud_data.hpp', 'hud_glyphs.hpp', 'hud_layout_data.hpp', 'hud_liquid_data.hpp', 'hud_limiter_data.hpp',
          'opening_credits_data.hpp', 'tutorial_prompt_glyphs.hpp', 'area_title_data.hpp', 'lore_tablet_layout.hpp')))
    graph.append(Rule('opening-blanker', (command('convert_opening_blanker'),),
                      (RESEARCH + 'opening-blanker',), ('include/opening_blanker_data.hpp',)))
    graph.append(Rule('lantern-glow', (command('convert_lantern_glow'),),
                      ('_private/research/lantern-glow', TEX3DS),
                      (CONVERTED+'lantern-glow/*', ROMFS+'/lantern-glow.bin', ROMFS+'/lantern-glow.t3x')))
    pack('lighting', 'convert_lighting', ('hero-light', 'hero-light-donut'), output='hero-light',
         extra=tuple(GAME_DATA + '/' + path for path in LIGHTING_INPUTS))
    graph.append(Rule('decor', (command('convert_decor'),),
                      (RESEARCH + 'room-visuals', RESEARCH + 'decompiled/all/Breakable.cs'),
                      (CONVERTED + 'decor/*', ROMFS + '/decor.bin')))
    pack('decor-effects', 'convert_decor_effects',
         ('decor-effects', 'room-visuals', 'plant-effects', 'grass-effects-source',
          'ambient-particles', 'ambient-particles-fungal', 'deepnest-ambient-particles', 'initial-fall', 'stalactite-sources', 'great-door-motion-source',
          'hero-light/level6-12820.json', 'breakable-owner-effects-v2', 'great-door',
          'great-door-vfx-source', 'great-door-visuals', 'platform-effects-source', 'terrain', 'tutorial'))
    # Keep the pre-resident RomFS separate: resident rebinding rewrites its banks.
    artwork = CONVERTED + 'room-plants'
    artwork_romfs = SCRATCH + 'room-artwork'
    baseline_stage = SCRATCH + 'room-baseline'
    graph.append(Rule('room-baseline', (command('convert_room_visuals',
        '--dynamics-source', RESEARCH + 'room-dynamics', '--tablet-glow-source', RESEARCH + 'tablet-glow',
        '--output', CONVERTED + 'room-visuals', '--romfs', baseline_stage),),
        tuple(RESEARCH + e for e in ('room-visuals', 'room-dynamics', 'tablet-glow', 'terrain',
                                    'assets/Physics2DSettings.json', 'hero-light/level6-12820.json', 'breakable-owner-effects-v2',
                                    'decompiled/all/PlayFromRandomFrameMecanim.cs')) + tutorial_scene_inputs() + (TEX3DS,),
        (CONVERTED + 'room-visuals/*', baseline_stage + '/*'), scratch=(baseline_stage,)))
    graph.append(Rule('room-artwork', (
        command('convert_room_visuals', '--plant-source', RESEARCH + 'plant-dependencies',
                '--grass-research', RESEARCH + 'grass-artwork-source',
                '--dynamics-source', RESEARCH + 'room-dynamics', '--tablet-glow-source', RESEARCH + 'tablet-glow',
                '--compact-tiles', '--quality-profile', 'new3ds-30fps', '--preserve-source-dimensions', '--max-pages', '64', '--texture-format', 'rgba4', '--output', artwork, '--romfs', artwork_romfs),
        command('room_dynamics_artwork', '--refresh-crumble', artwork)),
        tuple(RESEARCH + e for e in ('room-visuals', 'plant-dependencies', 'grass-artwork-source',
                                    'room-dynamics', 'collapse-crumble', 'tablet-glow', 'terrain', 'assets/Physics2DSettings.json',
                                    'hero-light/level6-12820.json',
                                    'breakable-owner-effects-v2', 'decompiled/all/PlayFromRandomFrameMecanim.cs')) + tutorial_scene_inputs() + (TEX3DS,),
        (artwork + '/*', artwork_romfs + '/*'), scratch=(artwork_romfs,)))
    resident = CONVERTED + 'room-plants-resident'
    room_romfs = CONVERTED + 'room-plants-romfs'
    stage = SCRATCH + 'room-resident'
    graph.append(Rule('room-resident', (command('convert_room_resident', '--source', artwork,
                                              '--output', resident, '--romfs', stage,
                                              '--max-texture-bytes',room_budget,'--required-texture-scale','1'),),
                      (artwork + '/*', TEX3DS,'include/render_memory.hpp','include/graphics_profile.hpp'), (resident + '/*', stage + '/*'),
                      after=('room-artwork',), copies=((stage + '/*', room_romfs),), scratch=(stage,)))
    graph.append(Rule('plants', (command('convert_plants'),),
                      (resident + '/*', room_romfs + '/room-resident-visual.bin',
                       RESEARCH + 'plant-effects', RESEARCH + 'room-visuals'), (room_romfs + '/plants.bin',),
                      after=('room-resident',)))
    banks = ('grass.bin', 'room-dynamics.bin', 'room-ambient.bin', 'plants.bin')
    graph.append(Rule('install-room', inputs=(resident + '/*', room_romfs + '/room-resident-*',
                      artwork_romfs + '/room-visual.bin', artwork_romfs + '/room-page-*.t3x') +
                      tuple(room_romfs + '/' + b for b in banks), after=('room-resident', 'plants'),
                      copies=((resident + '/*', CONVERTED + 'room-resident'),
                              (room_romfs + '/room-resident-*', ROMFS),
                              (artwork_romfs + '/room-visual.bin', ROMFS),
                              (artwork_romfs + '/room-page-*.t3x', ROMFS)) +
                      tuple((room_romfs + '/' + b, ROMFS) for b in banks)))
    static_decor = CONVERTED + 'tutorial-static'
    graph.append(Rule('room-Tutorial_01-static-decor', (
        command('static_decor_bake', '--bank', ROMFS + '/room-resident-visual.bin',
                '--page-directory', resident, '--page-prefix', 'room-resident-page-',
                '--source', RESEARCH + 'room-visuals', '--screen-scale', '2',
                '--bindings-directory', ROMFS, '--output', static_decor),),
        (resident + '/*', ROMFS + '/room-resident-*',
         RESEARCH + 'room-visuals', RESEARCH + 'terrain', TEX3DS,
         'include/render_memory.hpp') + tuple(ROMFS + '/' + b for b in banks),
        (static_decor + '/*',), after=('install-room',),
        copies=((static_decor + '/static-*.bin', ROMFS),
                (static_decor + '/static-room-page-*.t3x', ROMFS))))
    for name, script, source in (('false-knight-audio', 'convert_false_knight_audio', CONVERTED + 'false-knight-audio/source'),
                                  ('enemy-audio', 'convert_enemy_audio', CONVERTED + 'enemy/source'),
                                  ('wind-audio', 'convert_wind_audio', CONVERTED + 'wind/source'),
                                  ('music-audio', 'convert_music_audio', CONVERTED + 'music/source')):
        stage = SCRATCH + name
        audio_inputs = (source, FFMPEG) if (ROOT / FFMPEG).is_file() else (source,)
        if name == 'enemy-audio' and not (ROOT / source / 'sharedassets7-assets-106-switch_gate_switch.ogg').is_file():
            audio_inputs += ('_private/minimum-obstacles/town-lever-feedback/lever-hit.ogg',)
        graph.append(Rule(name, (command(script, '--output', stage),), audio_inputs,
                          (stage + '/*',), copies=((stage + '/*', ROMFS),), scratch=(stage,)))
    return graph


# Each external step lists its copied-original/session prerequisite, saved input
# snapshots and output snapshot. Flags distinguish extractors with several modes.
EXTRACTORS = {
    'stag': ('stag-stations', (), 'Copied original level9/dependencies/Managed; isolated AssetRipper started by extract_stag under validation-lock.ps1 -Exclusive'),
    'ambient_particles': ('ambient-particles', ('room-visuals',), 'AssetRipper; --mesh-only reads copied Unity default resources'),
    'area_title': ('area-title', ('tutorial-reminders', 'decompiled/all/StringEncrypt.cs'), 'AssetRipper'),
    'buzzer': ('buzzer', (), 'AssetRipper'),
    'conditional_events': ('conditional-events', ('tutorial-reminders',), 'AssetRipper'),
    'crawler': ('enemy', (), 'AssetRipper'),
    'decor_effects': ('decor-effects', ('room-visuals', 'decompiled/all'), 'AssetRipper and local audio decoder'),
    'fury_mask': ('fury-mask', ('room-visuals',), 'AssetRipper'),
    'fury_shiny': ('fury-chest', ('fury-chest',), 'AssetRipper'),
    'fury_tutorial': ('fury-reveals-source', ('fury-chest',), 'AssetRipper'),
    'geo_visuals': ('geo-visuals', ('stalactite-sources',), 'AssetRipper; --chips-only writes _private/converted/geo-chip-source'),
    'great_door_motion': ('great-door-motion-source', (), 'AssetRipper'),
    'great_door_visuals': ('great-door-visuals', ('great-door', 'room-visuals'), 'AssetRipper'),
    'health_scuttler': ('geo-visuals', ('geo-visuals',), 'AssetRipper for missing cached components'),
    'hero': ('assets', (), 'AssetRipper with copied _private/analysis-input and Managed'),
    'hero_damage_flash': ('hero-damage-flash', (), 'AssetRipper; --flings-only writes hero-damage-flings'),
    'hero_death': ('hero-death', (), 'AssetRipper'),
    'hero_effects': ('hero-effects', (), 'AssetRipper'),
    'focus_effects': ('focus-effects', (), 'AssetRipper with the copied Knight resources.assets'),
    'hud': ('hud', (), 'AssetRipper; --liquid/--layout/--feedback select sibling snapshots'),
    'map': ('bottom-map', (), 'AssetRipper; copied resources.assets with Managed'),
    'initial_fall': ('initial-fall', (), 'AssetRipper'),
    'lore_tablets': ('lore-tablets', ('tutorial-reminders',), 'AssetRipper'),
    'nail_interactions': ('nail-interactions', (), 'AssetRipper'),
    'opening_credits': ('opening-credits', ('tutorial-reminders',), 'AssetRipper'),
    'opening_blanker': ('opening-blanker', ('assets/c1-20600.json', 'decompiled/all/HeroController.cs'), 'AssetRipper'),
    'plant_effects': ('plant-effects', ('room-visuals',), 'AssetRipper'),
    'plant_hit_dependencies': ('plant-hit-dependencies-verified', (), 'AssetRipper; --verify is offline'),
    'plant_hit_visuals': ('plant-hit-visuals-verified', ('plant-hit-dependencies-verified',), 'AssetRipper; --verify is offline'),
    'plants': ('plants', ('room-visuals',), 'AssetRipper; also extends room-visuals'),
    'room_dynamics': ('room-dynamics', ('room-visuals',), 'AssetRipper; --crumble writes collapse-crumble'),
    'room_tilemap': ('room-visuals', (), 'AssetRipper'),
    'room_visuals': ('room-visuals', (), 'AssetRipper'),
    'stalactite_sources': ('stalactite-sources', ('room-visuals',), 'AssetRipper and local audio decoder'),
    'tablet_glow': ('tablet-glow', ('room-visuals',), 'AssetRipper'),
    'cornifer': ('cornifer', (), 'AssetRipper with copied Crossroads_33 (level64) and dependencies loaded'),
    'terrain': ('terrain', (), 'AssetRipper'),
    'tutorial_prompts': ('tutorial-reminders', ('assets/c1-20600.json', 'decompiled/all/StringEncrypt.cs'), 'AssetRipper and local .NET decrypt'),
}


def catalog():
    entries = [dict(tool='tools/extract_' + name + '.py', mode='external',
                    inputs=[RESEARCH + p for p in inputs], outputs=[RESEARCH + output], requires=requires)
               for name, (output, inputs, requires) in EXTRACTORS.items()]
    variants = {
        'geo_visuals': [CONVERTED + 'geo-chip-source'],
        'hero_damage_flash': [RESEARCH + 'hero-damage-flings'],
        'hud': [RESEARCH + name for name in ('hud-liquid', 'hud-layout', 'hud-feedback', 'hud-limiter')],
        'room_dynamics': [RESEARCH + 'collapse-crumble'],
        'plants': [RESEARCH + 'room-visuals'],
        'plant_effects': [RESEARCH + 'room-visuals'],
    }
    for entry in entries:
        name = entry['tool'].removeprefix('tools/extract_').removesuffix('.py')
        entry['outputs'].extend(variants.get(name, []))
    for name, script, inputs, outputs in (
        ('tutorial-audio-export', 'convert_tutorial_audio', ['AssetRipper', RESEARCH + 'assets/combat-map.json'],
         [RESEARCH + 'tutorial-audio', RESEARCH + 'plant-audio', ROMFS + '/sfx-*.wav', ROMFS + '/sfx-settings.bin']),
        ('enemy-audio-export', 'convert_enemy_audio', ['AssetRipper'], [CONVERTED + 'enemy/source']),
        ('wind-audio-export', 'convert_wind_audio', ['AssetRipper'], [CONVERTED + 'wind/source']),
    ):
        requires = 'AssetRipper; explicitly extract outside regen'
        if name == 'enemy-audio-export':
            requires += '; convert_enemy_audio.py --extract exports missing source OGGs before conversion'
        entries.append(dict(tool='tools/' + script + '.py', mode='external', name=name,
                            inputs=inputs, outputs=outputs, requires=requires))
    entries.extend([
        dict(tool='tools/room_breakable_audio_events.py', mode='external',
             inputs=['room-breakable-audio.json', 'captured raw gameplay states'],
             outputs=['independent room audio expectations'],
             requires='Called by build_audio_expectations.py; read-only validation helper'),
        dict(tool='tools/extract_city_lift_art.py', mode='external',
             inputs=['AssetRipper private level80 and dependencies'],
             outputs=['_private/minimum-obstacles/city-lift-art'],
             requires='Narrow original extraction under validation-lock.ps1 -Exclusive'),
        dict(tool='tools/extract_waterways_art.py', mode='external',
             inputs=['AssetRipper private level93 and dependencies'],
             outputs=['_private/minimum-obstacles/waterways-art'],
             requires='Narrow original extraction under validation-lock.ps1 -Exclusive'),
        dict(tool='tools/convert_waterways_machine.py', mode='helper',
             inputs=[RESEARCH+'crossroads-scenes/Ruins1_05b/visuals',
                     '_private/minimum-obstacles/waterways-art', RESEARCH+'tutorial-reminders/EN_Prompts-decrypted.xml'],
             outputs=[ROMFS+'/scenes/Ruins1_05b/waterways-machine.bin'],
             requires='Owned by scene-Ruins1_05b when its saved scene is available'),
        dict(tool='tools/convert_waterways_art.py', mode='helper',
             inputs=['_private/minimum-obstacles/waterways-art', TEX3DS],
             outputs=[ROMFS+'/scenes/Ruins1_05b/waterways-art*'],
             requires='Owned by scene-Ruins1_05b when its saved scene is available'),
        dict(tool='tools/extract_hazard_respawns.py', mode='external',
             inputs=['_private/original/Hollow Knight/hollow_knight_Data',
                     RESEARCH+'crossroads-scenes/Crossroads_16/terrain',
                     RESEARCH+'crossroads-scenes/Crossroads_11_alt/terrain',
                     RESEARCH+'crossroads-scenes/Fungus1_01/terrain',
                     RESEARCH+'crossroads-scenes/Fungus1_02/terrain',
                     RESEARCH+'crossroads-scenes/Fungus1_07/terrain'],
             outputs=['_private/minimum-obstacles/respawn-markers/Crossroads_11_alt',
                      '_private/converted-psp/route-scenes-source/Crossroads_16-markers',
                      '_private/minimum-obstacles/respawn-markers/Fungus1_01',
                      '_private/minimum-obstacles/respawn-markers/Fungus1_02',
                      '_private/minimum-obstacles/respawn-markers/Fungus1_07'],
             requires='Isolated original marker dependency recovery under validation-lock.ps1 -Exclusive; no canonical export edits'),
        dict(tool='tools/extract_stag_station_art.py', mode='external',
             inputs=['AssetRipper private Crossroads_47 and dependencies'],
             outputs=['_private/minimum-obstacles/stag-station-art'],
             requires='Narrow original extraction under validation-lock.ps1 -Exclusive'),
        dict(tool='tools/extract_world_lift.py', mode='external',
             inputs=['AssetRipper private Town and dependencies'],
             outputs=['_private/minimum-obstacles/town-art'],
             requires='Narrow original extraction under validation-lock.ps1 -Exclusive'),
        dict(tool='tools/extract_world_switch.py', mode='external',
             inputs=['AssetRipper private level39 and dependencies'],
             outputs=['_private/minimum-obstacles/switch-art'],
             requires='Explicit isolated extraction under validation-lock -Exclusive'),
        dict(tool='tools/extract_town_lever_feedback.py', mode='external',
             inputs=['_private/original/Hollow Knight/hollow_knight_Data',
                     RESEARCH+'crossroads-scenes/Town/terrain/level7-3919.json'],
             outputs=['_private/minimum-obstacles/town-lever-feedback'],
             requires='Isolated original feedback extraction under validation-lock.ps1 -Exclusive'),
        dict(tool='tools/extract_stag_feedback.py', mode='external',
             inputs=['copied build22529139 level244 and referenced audio/material/texture dependencies',
                     '_private/minimum-obstacles/resting-station-art'],
             outputs=['_private/minimum-obstacles/resting-station-feedback'],
             requires='Targeted immutable original recovery under validation-lock.ps1 -Exclusive'),
        dict(tool='tools/extract_world_switch_session.py', mode='external',
             inputs=['_private/original/Hollow Knight/hollow_knight_Data',
                     RESEARCH+'crossroads-scenes/Crossroads_03/visuals'],
             outputs=['_private/minimum-obstacles/switch-art'],
             requires='Isolated recovery under validation-lock.ps1 -Exclusive; stops its server in finally'),
        dict(tool='tools/extract_mantis_gate_art.py', mode='external',
             inputs=['_private/original/Hollow Knight/hollow_knight_Data',
                     RESEARCH+'crossroads-scenes/Fungus2_14/visuals',
                     RESEARCH+'crossroads-scenes/Fungus2_15/visuals'],
             outputs=['_private/minimum-obstacles/mantis-14-room',
                      '_private/minimum-obstacles/mantis-15-room'],
             requires='Targeted immutable original recovery under validation-lock.ps1 -Exclusive; stops its server in finally'),
        dict(tool='tools/convert_world_lifts.py', mode='inactive',
             inputs=[RESEARCH+'crossroads-scenes/Town/terrain/level7-4026.json'],
             outputs=[CONVERTED+'minimum-obstacles/mine-lift.bin'],
             requires='Strict Lift Control helper; live Town bank uses convert_town_lift'),
        dict(tool='tools/extract_city_crest_respawn.py', mode='external',
             inputs=['copied build22529139 level186 and dependencies'],
             outputs=['_private/minimum-obstacles/city-crest-source'],
             requires='Isolated source recovery under validation-lock.ps1 -Exclusive; always stops its server'),
        dict(tool='tools/extract_city_crest_effects.py', mode='external',
             inputs=['copied build22529139 level186 and material/texture dependencies'],
             outputs=['_private/minimum-obstacles/city-crest-effects', '_private/minimum-obstacles/city-crest-rocks'],
             requires='Targeted immutable snapshot and original texture recovery under -Exclusive; --rocks recovers Bump debris separately'),
        dict(tool='tools/extract_city_crest_slam_art.py', mode='external',
             inputs=['copied build22529139 level186, Managed and referenced tk2d resources'],
             outputs=['_private/minimum-obstacles/city-crest-slam-art'],
             requires='Targeted immutable tk2d recovery under validation-lock.ps1 -Exclusive'),
        dict(tool='tools/extract_city_crest_audio.py', mode='external',
             inputs=['copied build22529139 level186, sharedassets6.assets and sharedassets7.assets'],
             outputs=['_private/minimum-obstacles/city-crest-audio'],
             requires='Targeted original WAV recovery under validation-lock.ps1 -Exclusive'),
        dict(tool='tools/convert_world_key_doors.py', mode='inactive',
             inputs=[RESEARCH+'crossroads-scenes/Town/terrain/level7-4067.json'],
             outputs=[CONVERTED+'minimum-obstacles/jiji-door.bin'],
             requires='Explicit private conversion; production dialogue/tween/entrance/save adapters pending'),
        dict(tool='tools/extract_scene_grade.py', mode='external', inputs=['private original level file'],
             outputs=[RESEARCH+'crossroads-scenes'],
             requires='UnityPy + verified component/transform IDs, or --controls --scene --output from a verified original-player capture; validation-lock.ps1 -Exclusive'),
        dict(tool='tools/extract_room_tk2d.py', mode='external',
             inputs=[RESEARCH+'crossroads-scenes', 'AssetRipper copied original room scenes'],
             outputs=[RESEARCH+'crossroads-scenes/*/visuals/tk2d'],
             requires='Explicit --source visuals directory and --port with original room dependencies loaded; validation-lock.ps1 -Exclusive'),
        dict(tool='tools/extract_ruins_gate_art.py', mode='external',
             inputs=[RESEARCH+'crossroads-scenes/Ruins1_05', RESEARCH+'crossroads-scenes/Ruins1_05b', RESEARCH+'crossroads-scenes/Ruins1_05c', 'copied build22529139 level92/93/94 and their dependencies'],
             outputs=['_private/minimum-obstacles/ruins-gate-room', '_private/minimum-obstacles/ruins-gate-05c-room', '_private/minimum-obstacles/ruins-gate-05-room'],
             requires='Isolated original AssetRipper session; validation-lock.ps1 -Exclusive; shared scene exports remain read only'),
        dict(tool='tools/extract_mantis_gate_room.py', mode='external',
             inputs=[RESEARCH+'crossroads-scenes/Fungus2_18', 'copied build22529139 level183 and its dependencies'],
             outputs=['_private/minimum-obstacles/mantis-18-room'],
             requires='Isolated original AssetRipper session; validation-lock.ps1 -Exclusive; retains existing room artwork'),
        dict(tool='tools/extract_false_knight_effects.py', mode='external',
             inputs=['copied build22529139 level48 and its texture dependencies'],
             outputs=[RESEARCH+'false-knight-effects'],
             requires='UnityPy in --packages; copied --source; validation-lock.ps1 -Exclusive'),
        dict(tool='tools/extract_scene_borders.py', mode='external', inputs=['AssetRipper private room scenes'],
             outputs=[RESEARCH+'crossroads-scenes'],
             requires='Explicit --scene --port with the source room loaded; validation-lock.ps1 -Exclusive'),
        dict(tool='tools/convert_ability_pickups.py', mode='external', inputs=['AssetRipper private ability rooms'],
             outputs=[RESEARCH+'ability-pickups'], requires='Explicit --extract --port with the copied original loaded'),
        dict(tool='tools/extract_room_blur.py', mode='external', inputs=['AssetRipper private room scenes'],
             outputs=[RESEARCH+'room-visuals', RESEARCH+'crossroads-scenes'],
             requires='Backfill one room with --source and either --collection --port or --scene-asset <copied level>; preserves valid tk2d anchors; validation-lock.ps1 -Exclusive'),
        dict(tool='tools/extract_mantis_scenery.py', mode='external',
             inputs=['_private/original/Hollow Knight/hollow_knight_Data/level180',
                     '_private/original/Hollow Knight/hollow_knight_Data/sharedassets*.assets*'],
             outputs=[RESEARCH+'crossroads-scenes/Fungus2_15/visuals/level180',
                      RESEARCH+'ambient-particles-fungal'],
             requires='Export native Mantis arena scenery under validation-lock.ps1 -Exclusive; scene-Fungus2_15 consumes the saved export'),
        dict(tool='tools/extract_scenes.py', mode='external', inputs=['private original scene files'],
             outputs=[RESEARCH+'crossroads-scenes', '_private/converted-psp/route-scenes-source/Dream_Nailcollection'], requires='Isolated AssetRipper port 58292 with the requested --inventory scene batch and dependencies loaded; extract_scenes.py --inventory <inventory.json> --region <region> under validation-lock.ps1 -Exclusive; Dream_Nailcollection uses --output-root _private/converted-psp/route-scenes-source'),
        dict(tool='tools/restore_crossroads_enemy_research.py', mode='external',
             inputs=['copied build22529139 original files loaded in AssetRipper'],
             outputs=[RESEARCH + 'crossroads-enemies-a', RESEARCH + 'crossroads-enemies-b'],
             requires='Use --only enemy-a or enemy-b --source <copied hollow_knight_Data> --port <port> under validation-lock.ps1 -Exclusive'),
        dict(tool='tools/convert_crossroads_bosses.py', mode='external',
             inputs=['_private/original/Hollow Knight/hollow_knight_Data', '_private/tools/boss-python'],
             outputs=[RESEARCH + 'crossroads-bosses', RESEARCH + 'false-knight-fullres'],
             requires='Use --save-snapshot with copied original assets, UnityPy and TypeTreeGeneratorAPI; add --false-knight-only --scale 1 for false-knight-fullres; snapshot conversion is active'),
        dict(tool='tools/convert_room_chunks.py', mode='inactive', inputs=[CONVERTED + 'room-visuals'],
             outputs=[CONVERTED + 'room-chunks', ROMFS + '/room-*-*-*'],
             requires='Obsolete streamed room alternative; active package uses resident room'),
        dict(tool='tools/convert_terrain.py', mode='helper', inputs=[RESEARCH + 'terrain'],
             outputs=[CONVERTED + 'terrain', ROMFS + '/tutorial.bin'],
             requires='observe/require helpers used by active converters; standalone is a diagnostic slice'),
    ])
    return entries


def validate_catalog(root, graph):
    ordered(graph)
    covered = {item['tool'] for item in catalog()}
    visited = set()
    for rule in graph:
        scripts = tuple(a for cmd in rule.commands for a in cmd if a.endswith('.py')) + rule.modules
        if scripts not in visited:
            visited.add(scripts)
            covered.update(local_modules(root, scripts))
    actual = {p.relative_to(root).as_posix() for pattern in ('convert_*.py', 'extract_*.py', 'room_*.py')
              for p in (root / 'tools').glob(pattern)}
    if actual - covered:
        raise ValueError('Register new asset tools in regen_manifest.py: ' + ', '.join(sorted(actual - covered)))
