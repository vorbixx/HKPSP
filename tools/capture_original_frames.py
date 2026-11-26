"""Capture controlled effects or ambient grass in a private original player copy."""
import converted_files  # Install hardlink copy-on-write before asset writes.
import argparse
import csv
import json
import math
import re
import shutil
from pathlib import Path

from PIL import Image, ImageChops
from probe_unity_physics import run, digest
from verify_probe_sources import verify
from compare_decor_particles import validate_snapshots
from compare_decor_layers import validate_observations
from extract_scenes import SCENES

ROOT = Path(__file__).resolve().parents[1]
REFERENCES = ROOT / '_private/research/reference-frames'
# Worktrees share research, while converted stays local to each checkout.
LAPTOP_REFERENCES = (ROOT / '_private/research').resolve().parent / 'laptop-reference-frames'
ROOM_VIEW_SCENE_ASSETS = {'Room_Town_Stag_Station': 9,
                          'Town': 7,
                          'Room_shop': 17,
                          'Room_mapper': 12,
                          'Fungus2_11': 175,
                          'Fungus2_14': 178,
                          'Fungus2_15': 179,
                          'Fungus2_18': 183,
                          'Crossroads_01': 37,
                          'Crossroads_02': 38,
                          'Crossroads_03': 39,
                          'Crossroads_06': 42,
                          'Crossroads_07': 43,
                          'Crossroads_10': 46,
                          'Crossroads_11_alt': 50,
                          'Crossroads_13': 52,
                          'Crossroads_47': 77,
                          'Crossroads_49b': 80,
                          'Crossroads_50': 81,
                          'Fungus1_01': 128,
                          'Fungus1_02': 130,
                          'Fungus1_03': 131,
                          'Fungus1_04': 132,
                          'Fungus1_05': 134,
                          'Fungus1_06': 135,
                          'Fungus1_07': 136,
                          'Fungus1_30': 157,
                          'Fungus1_31': 158,
                          'Fungus1_17': 146,
                          'Crossroads_15': 54,
                          'Fungus1_13': 142, 'Fungus1_21': 149, 'Fungus1_28': 155, 'Fungus1_31': 158,
                          'Fungus2_03': 167, 'Fungus2_05': 169, 'Fungus2_15': 179, 'Fungus2_21': 186, 'Fungus2_23': 187, 'Ruins1_02': 89, 'Ruins1_03': 90, 'Ruins1_04': 91,
                          'Ruins1_05': 92, 'Ruins1_05b': 93, 'Ruins1_05c': 94, 'Ruins1_25': 104, 'Ruins1_31': 108,
                          'Ruins2_04': 116, 'Ruins2_05': 117,
                          'Ruins2_01_b': 112, 'Ruins2_03b': 114, 'Ruins2_Watcher_Room': 127, 'Ruins_Elevator': 86,
                          'Abyss_01': 327, 'Abyss_02': 328, 'Abyss_03': 329, 'Abyss_04': 332,
                          'Abyss_09': 336, 'Abyss_18': 342, 'Abyss_19': 343, 'Abyss_21': 345,
                          'RestingGrounds_09': 244, 'Waterways_05': 354, 'Waterways_07': 357, 'Waterways_13': 362,
                          'Fungus3_archive_02': 228, 'Dream_Guardian_Monomon': 402, 'Cutscene_Boss_Door': 403,
                          'Deepnest_10': 282, 'Deepnest_16': 284, 'Deepnest_26': 286,
                          'Deepnest_30': 288, 'Deepnest_34': 292, 'Deepnest_41': 299,
                          'Tutorial_01': 6, 'Crossroads_47': 77, 'Crossroads_49b': 80, 'Mines_33': 272}
POSTPROCESS_VARIANTS = ('rerender-normal', 'bloom-off', 'bloom-off-uncorrected',
                        'bloom-off-uncorrected-unlit', 'restored-normal')
SCENE_VARIANTS = tuple('raw-no-' + kind for kind in ('owner', 'dust', 'flash', 'debris'))


def room_view_output_allowed(references, converted, swim_private, *, active_swim=False, laptop_root=None):
    """Keep reference captures in the explicitly designated private roots."""
    return (references.is_relative_to(converted)
            or (active_swim and references == swim_private/'references')
            or (laptop_root is not None and references.is_relative_to(laptop_root.resolve())))


def validate_vignette_scale(scale, name, expected):
    if not math.isfinite(scale) or scale <= 0:
        raise ValueError('Invalid vignette scale: ' + name)
    if expected is not None and (not math.isfinite(expected) or expected <= 0):
        raise ValueError('Expected vignette scale must be finite and positive')
    if expected is not None and abs(scale - expected) > .0001:
        raise ValueError(f'Vignette scale mismatch: {name}: {scale} != {expected}')


def selected_scene_assets(data, scene_index, source_files=()):
    """Copy the scene and shared asset bundles recorded by its source manifest."""
    names = [f'level{scene_index}']
    bundles = set()
    for item in source_files:
        if item.startswith('sharedassets'):
            bundle = item.split('-', 1)[0]
            if bundle.endswith('.assets'):
                bundles.add(bundle)
    for bundle in sorted(bundles):
        names.append(bundle)
        names.extend(suffix for suffix in (bundle + '.resS', bundle.removesuffix('.assets') + '.resource')
                     if (data / suffix).is_file())
    names = list(dict.fromkeys(names))
    missing = [name for name in names if not (data / name).is_file()]
    if missing:
        raise FileNotFoundError('Missing original scene assets: ' + ', '.join(missing))
    return names


def validate_lighting(directory, expected, hero, camera, vignette_scale=None):
    """A fixed pose must retain physics triggers and the settled room lighting."""
    if any(len(v) != 3 or not all(math.isfinite(a) for a in v) for v in (hero, camera)):
        raise ValueError('Expected lighting fixture poses must have three finite coordinates')
    if vignette_scale is not None and (not math.isfinite(vignette_scale) or vignette_scale <= 0):
        raise ValueError('Expected vignette scale must be finite and positive')
    for name in ['before'] + [f'frame-{frame:03}' for frame in expected]:
        state = json.loads((directory / f'lighting-{name}.json').read_text())
        if state['hero_simulated'] is not True or state['hero_frozen'] is not True:
            raise ValueError('Lighting fixture must retain frozen hero physics: ' + name)
        for key, value in [('hero_position', hero), ('camera_position', camera)]:
            actual = state[key]
            if len(actual) != 3 or any(not math.isfinite(a) or abs(a - b) > .0001
                                       for a, b in zip(actual, value)):
                raise ValueError('Lighting fixture pose drift: ' + name + '/' + key)
        validate_vignette_scale(state['vignette_scale'], name, vignette_scale)


def validate_sphere_hit(directory):
    """Require actual source damage after entering the authored Sphere state."""
    fixture = json.loads((directory / 'hornet-fixture.json').read_text())
    with (directory / 'frames.csv').open(newline='') as stream:
        rows = list(csv.DictReader(stream))
    if fixture['sourceState'] != 'Sphere' or not rows or min(int(row['heroHealth']) for row in rows) >= fixture['startHealth']:
        raise ValueError('Sphere fixture did not record source hero damage')


def validate_scene_controls(directory, expected, size):
    """Exclusions must target only the observed owner's five renderers."""
    names = ['before'] + ['frame-' + format(f, '03') for f in expected]
    for name in names:
        renderers = json.loads((directory / f'control-{name}-scene-renderers.json').read_text())
        kinds = [row['kind'] for row in renderers]
        if (kinds.count('debris') != 2 or set(kinds) - {'debris', 'dust', 'flash'}
                or (name != 'before' and (kinds.count('dust') != 1 or kinds.count('flash') != 2))):
            raise ValueError('Unexpected owner scene renderers: ' + name)
        with Image.open(directory / f'control-{name}-bloom-off-uncorrected-full.png') as raw:
            for variant in SCENE_VARIANTS:
                with Image.open(directory / f'control-{name}-{variant}-full.png') as image:
                    if image.size != size or not image.getbbox():
                        raise ValueError('Invalid scene control image: ' + name + '/' + variant)
                    if name == 'before' and ImageChops.difference(image, raw).getbbox():
                        raise ValueError('Scene exclusion changed the before image: ' + variant)


def validate_lifeblood_heart_fixture(args):
    if (getattr(args, 'lifeblood_heart', False) and
            (args.scene != 'Tutorial_01' or not getattr(args, 'room_view', False) or not getattr(args, 'stable_room', False))):
        raise ValueError('Lifeblood Heart reference requires stable Tutorial_01 room view')


def validate_gate_resources_fixture(args):
    if not getattr(args, 'gate_resources', False):
        return
    if (args.scene != 'Town' or not getattr(args, 'room_view', False) or
            getattr(args, 'stable_room', False) or getattr(args, 'hazard_point', None) or
            getattr(args, 'frames', [0]) != [0]):
        raise ValueError('Gate resources require a live Town room view')


def validate_gate_resource_trace(path):
    with path.open(newline='') as stream:
        rows = list(csv.DictReader(stream))
    if (len(rows) != 2 or [r['phase'] for r in rows] != ['before', 'after'] or
            [r['scene'] for r in rows] != ['Town', 'Tutorial_01'] or
            any((int(r['health']), int(r['soul']), int(r['reserve'])) != (2, 44, 0) for r in rows) or
            rows[-1]['controllerEnabled'] != 'True'):
        raise ValueError('Gate trace must retain live health/Soul/reserve through Town left1')


def validate_stable_room_state(path):
    state = json.loads(path.read_text(encoding='utf-8'))
    if (state.get('facing') != 'right' or state.get('animation') != 'Idle' or
            type(state.get('health')) is not int or type(state.get('maxHealth')) is not int or
            state['health'] <= 0 or state['health'] != state['maxHealth'] or
            state.get('grounded') is not True or
            not math.isfinite(state.get('normalizedTime', 0))):
        raise ValueError('Stable room state is not a grounded, full-health, right-facing idle hero')


def frames_argument(value):
    frames = [int(f) for f in value.split(',')]
    if not frames or frames != sorted(set(frames)) or frames[0] < 0 or frames[-1] > 3600:
        raise argparse.ArgumentTypeError('Use unique ascending frame offsets in 0..3600')
    return frames


def validate_bretta_first_dialogue(scene, target, observe):
    if scene != 'Fungus2_23' or target != 'Bretta Dazed' or not observe:
        raise ValueError('--bretta-first-dialogue requires --scene Fungus2_23 --target "Bretta Dazed" --observe')


def validate_bretta_first_dialogue_evidence(evidence):
    expected = {'fsm': 'npc_control', 'event': 'UP PRESSED', 'state_before': 'Idle',
                'state_after': 'Meet', 'conversation': 'BRETTA_RESCUE_1', 'language': 'EN'}
    if (any(evidence.get(key) != value for key, value in expected.items())
            or type(evidence.get('page')) is not int or evidence['page'] != 1
            or evidence.get('typing') is not False or evidence.get('grounded') is not True):
        raise ValueError('Bretta capture requires the grounded authored interaction and fully revealed first page')


def validate_sly_first_dialogue(scene, target, observe):
    if scene != 'Room_ruinhouse' or target != 'Sly Dazed' or not observe:
        raise ValueError('--sly-first-dialogue requires --scene Room_ruinhouse --target "Sly Dazed" --observe')


def validate_sly_awake(scene, target, observe):
    if scene != 'Room_ruinhouse' or target != 'Sly Dazed' or not observe:
        raise ValueError('--sly-awake requires --scene Room_ruinhouse --target "Sly Dazed" --observe')


def validate_sly_title(scene, target, observe):
    if scene != 'Room_ruinhouse' or target != 'Sly Dazed' or not observe:
        raise ValueError('--sly-title requires --scene Room_ruinhouse --target "Sly Dazed" --observe')


def validate_sly_first_dialogue_language(language):
    if language != 'EN':
        raise ValueError('Sly first-dialogue capture must select English (EN)')


def validate_sly_awake_evidence(evidence):
    validate_sly_first_dialogue_language(evidence.get('language'))
    expected = {'interaction_fsm': 'npc_control', 'interaction_event': 'UP PRESSED',
                'conversation': 'SLY_RESCUE_1', 'completion': 'DialoguePageControl.Input',
                'state_before': 'Idle', 'state_after': 'Meet 2'}
    if (any(evidence.get(key) != value for key, value in expected.items())
            or type(evidence.get('completed_pages')) is not int
            or evidence['completed_pages'] < 1):
        raise ValueError('Sly awake fixture must complete the authored first conversation')


def validate_sly_title_evidence(evidence):
    validate_sly_first_dialogue_language(evidence.get('language'))
    expected = {'interaction_fsm': 'npc_control', 'interaction_event': 'UP PRESSED',
                'conversation': 'SLY_RESCUE_3', 'state_after': 'Meet 3',
                'title_area_event': 'SLY', 'npc_title': True}
    if (any(evidence.get(key) != value for key, value in expected.items())
            or type(evidence.get('completed_pages')) is not int
            or evidence['completed_pages'] < 1):
        raise ValueError('Sly title fixture must complete the authored rescue conversations and enter the SLY title state')
def verify_archived_source(provenance, archived):
    """Check the archived player against the prior, build-stamped source capture."""
    archived = archived.resolve()
    prior = ROOT / '_private/tutorial-intro-probe/results/provenance.json'
    report = ROOT / '_private/tutorial-intro-probe/results/intro-report.json'
    build = json.loads(report.read_text())['build']
    if build != 22529139:
        raise ValueError(f'Archived source build changed: {build}')
    source_hashes = json.loads(prior.read_text())['source_sha256']
    files = []
    for name, expected in source_hashes.items():
        path = (ROOT / name.replace('\\', '/')).resolve()
        if not path.is_relative_to(archived):
            continue
        if digest(path) != expected:
            raise ValueError(f'Archived original input changed: {path.relative_to(archived)}')
        files.append(dict(file=path.relative_to(archived).as_posix(), sha256=expected, matches=True))
    if not files:
        raise ValueError('No archived player inputs in prior source provenance')
    return dict(build=build, method='Read-only archived-copy hash comparison with prior build-stamped Tutorial_01 capture',
                files=files, validated=True, live_steam_install_present=False)


def validate_grass(directory, expected, target=None):
    """Retain all owners/clocks and require the compared owner to stay idle."""
    baseline = None
    epoch = None
    for offset in [None, *expected]:
        name = 'before' if offset is None else f'frame-{offset:03}'
        state = json.loads((directory / f'grass-{name}.json').read_text())
        clocks = [state[k] for k in ('time', 'levelTime', 'shaderTime')]
        if not all(math.isfinite(v) for v in clocks) or abs(clocks[1] - clocks[2]) > .00005:
            raise ValueError('Grass shader clock is not level time: ' + name)
        rows = state['renderers']
        active = [r for r in rows if r['active']]
        if len(rows) != 82 or len(active) != 41 or len({r['owner'] for r in active}) != 41:
            raise ValueError('Expected all 41 ambient grass owners: ' + name)
        pose = {}
        for row in rows:
            if len(row['matrix']) != 16 or not all(math.isfinite(v) for v in [*row['matrix'], row['push']]):
                raise ValueError('Invalid ambient grass pose: ' + name)
            if target is None or row['owner'] == target or row['path'].startswith(target + '/'):
                if row['active'] and not row['sprite'].endswith('_00000'):
                    raise ValueError('Ambient fixture contains a grass interaction: ' + name)
                pose[row['path']] = (row['matrix'], row['push'], row['sprite'], row['active'])
        if not pose:
            raise ValueError('Grass comparison target is missing: ' + str(target))
        if baseline is not None and pose != baseline:
            raise ValueError('Ambient fixture contains a grass interaction: ' + name)
        baseline = pose
        if offset is not None:
            if epoch is None:
                epoch = state['shaderTime'] - offset / 60
            if abs(state['shaderTime'] - epoch - offset / 60) > .00005:
                raise ValueError('Grass capture clock drift: ' + name)


def validate_frames(directory, expected, size):
    with (directory / 'frames.csv').open() as stream:
        rows = list(csv.DictReader(stream))
    if [int(r['offset']) for r in rows] != expected:
        raise ValueError('Missing frame offsets')
    epoch = int(rows[0]['unityFrame']) - expected[0]
    if any(int(r['unityFrame']) - epoch != int(r['offset']) for r in rows):
        raise ValueError('Unity frame offset drift')
    for name in ['before'] + ['frame-' + format(f, '03') for f in expected]:
        with Image.open(directory / (name + '-full.png')) as im:
            if im.size != size or not im.getbbox():
                raise ValueError('Invalid full size capture')
    errors = directory / 'runtime-errors.txt'
    if errors.exists() and errors.read_text().strip():
        raise ValueError('Runtime errors during hit capture')
    return rows


def room_ruinhouse_extra_assets(data):
    # The Sly animator references sharedassets17.assets-217 (Wake included),
    # while the room's local objects live in level20/sharedassets20.
    return ['level20'] + [p.name for p in data.iterdir()
                          if p.name.startswith(('sharedassets17.', 'sharedassets20.'))]


def validate_postprocess_controls(directory, expected, size, *, include_before=True):
    names = (['before'] if include_before else []) + ['frame-' + format(f, '03') for f in expected]
    rows = [json.loads(line) for line in (directory / 'postprocess-controls.jsonl').read_text().splitlines()]
    if [row['capture'] for row in rows] != names:
        raise ValueError('Missing postprocess control captures')
    for name, row in zip(names, rows):
        if row['stateBefore'] != row['stateAfter']:
            raise ValueError('Postprocess control simulation or pose drift: ' + name)
        if row['effectScalarStateBefore'] != row['effectScalarStateAfter']:
            raise ValueError('Postprocess effect scalar fields drift: ' + name)
        if not row['restoredNormal']['exact']:
            raise ValueError('Postprocess restored normal drift: ' + name)
        images = {}
        for variant in POSTPROCESS_VARIANTS:
            with Image.open(directory / f'control-{name}-{variant}-full.png') as image:
                if image.size != size or not image.getbbox():
                    raise ValueError('Invalid postprocess control image: ' + name + '/' + variant)
                images[variant] = image.convert('RGB')
        restored = ImageChops.difference(images['rerender-normal'], images['restored-normal'])
        if restored.getbbox():
            raise ValueError('Postprocess restored normal pixels differ: ' + name)
        with Image.open(directory / (name + '-full.png')) as screenshot:
            difference = ImageChops.difference(screenshot.convert('RGB'), images['rerender-normal'])
        exact = difference.getbbox() is None
        if row['screenshotComparison']['exact'] != exact:
            raise ValueError('Postprocess screenshot comparison metadata differs: ' + name)
    return rows


def scene_assets(source, level):
    """Copy the requested built-in scene and its authored asset references."""
    if level is None:
        return ()
    if level < 0:
        raise ValueError('Scene level must be nonnegative')
    scene = f'level{level}'
    dependencies = set(re.findall(rb'sharedassets[0-9]+\.assets', (source / scene).read_bytes()))
    names = {scene}
    for dependency in dependencies:
        name = dependency.decode('ascii')
        if not (source / name).is_file():
            raise ValueError('Missing source scene dependency: ' + name)
        names.update(p.name for p in source.glob(name.removesuffix('.assets') + '.*') if p.is_file())
    return tuple(sorted(names))


def validate_town_lift_fixture(args):
    if getattr(args, 'town_lift_lever', False) and (
            not getattr(args, 'room_view', False) or not getattr(args, 'stable_room', False)
            or args.scene != 'Town' or getattr(args, 'jiji_door', None)):
        raise ValueError('Town lift lever fixture requires stable Town room view without Jiji')


def validate_ruins_lever_fixture(args):
    if not getattr(args, 'ruins_lever', False):
        return
    if (not args.room_view or not args.stable_room or args.scene not in ('Ruins1_05', 'Ruins1_05b', 'Ruins1_05c')
            or any(getattr(args, key, False) for key in
                   ('waterways_machine', 'town_lift_lever', 'jiji_door', 'lift_motion'))):
        raise ValueError('Ruins lever fixture requires stable Ruins1_05/05b/05c room view without other interactions')


def validate_stag_lever_fixture(args):
    if not getattr(args, 'resting_stag_lever', False):
        return
    if (not getattr(args, 'room_view', False) or not getattr(args, 'stable_room', False)
            or args.scene != 'RestingGrounds_09'):
        raise ValueError('Stag lever fixture requires stable RestingGrounds_09 room view')


def validate_hazard_trace(path, expected):
    """Reject static/failed source fixtures before admitting a respawn image."""
    with path.open(newline='') as stream:
        rows = list(csv.DictReader(stream))
    if not rows or [int(r['step']) for r in rows] != list(range(-1, len(rows)-1)):
        raise ValueError('Hazard trace must contain contiguous source frames from baseline')
    returned = next((r for r in rows if int(r['step']) == 240), None)
    if returned is None:
        raise ValueError('Hazard trace is missing the source return frame')
    before = int(rows[0]['health'])
    if before < 2 or min(int(r['health']) for r in rows) != before-1 or int(returned['health']) != before-1:
        raise ValueError('Hazard trace must lose one mask and remain alive')
    pose = [float(returned[key]) for key in ('heroX', 'heroY')]
    if any(not math.isfinite(v) or abs(v-want) > .001 for v, want in zip(pose, expected[:2])):
        raise ValueError('Hazard trace missed the expected source return position')
    for r in (rows[0], returned):
        gravity = float(r['gravity'])
        if (r['controllerEnabled'] != 'True' or r['simulated'] != 'True' or
                r['constraints'] != 'None' or not math.isfinite(gravity) or gravity <= 0):
            raise ValueError('Hazard trace lost live source controller/physics')


def validate_greenpath_toll_fixture(args):
    if not getattr(args, 'greenpath_toll_input', False):
        return
    if not args.room_view or args.stable_room or args.scene != 'Fungus1_31':
        raise ValueError('Greenpath toll input requires live Fungus1_31 room view')
    if 0 not in args.frames or max(args.frames) < 300:
        raise ValueError('Greenpath toll input requires before frame 0 and a frame at or after 300')


def validate_greenpath_toll_trace(path):
    with path.open(newline='') as source:
        rows = list(csv.DictReader(source))
    if (len(rows) < 2 or int(rows[0]['offset']) != 0 or
            int(rows[-1]['offset']) < 300 or
            int(rows[0]['geo']) - int(rows[-1]['geo']) != 50 or
            rows[0]['grounded'] != 'True' or rows[0]['canTalk'] != 'True'):
        raise ValueError('Greenpath toll trace did not establish a live 50 Geo payment')


def capture_room_view(args):
    """Capture a settled original room with authentic in-scene prompts."""
    greenpath_toll = getattr(args, 'greenpath_toll_input', False)
    validate_greenpath_toll_fixture(args)
    active_swim = getattr(args, 'surface_swim', False)
    mantis_challenge = getattr(args, 'mantis_challenge', False)
    if mantis_challenge and (not args.room_view or args.scene != 'Fungus2_15' or active_swim or getattr(args, 'stable_room', False)):
        raise ValueError('Mantis challenge requires a live Fungus2_15 room view')
    lift_motion = getattr(args, 'lift_motion', False)
    jiji = getattr(args, 'jiji_door', None)
    city_crest = getattr(args, 'city_crest_moment', None)
    if city_crest and (not args.room_view or not args.stable_room or args.scene != 'Fungus2_21'):
        raise ValueError('City Crest fixture requires stable Fungus2_21 room view')
    city_toll = getattr(args, 'city_lift_toll', False)
    if city_toll and (not args.room_view or not args.stable_room or args.scene!='Crossroads_49b'):
        raise ValueError('City toll fixture requires stable Crossroads_49b room view')
    town_lever = getattr(args, 'town_lift_lever', False)
    waterways = getattr(args, 'waterways_machine', False)
    crossroads_switch = getattr(args, 'crossroads_switch', False)
    if crossroads_switch and (not args.room_view or args.scene != 'Crossroads_03'
                              or waterways or town_lever or jiji or lift_motion):
        raise ValueError('Crossroads switch fixture requires Crossroads_03 room view')
    if waterways and (not args.room_view or not args.stable_room or args.scene != 'Ruins1_05b'
                      or town_lever or jiji or lift_motion):
        raise ValueError('Waterways machine fixture requires stable Ruins1_05b room view')
    validate_town_lift_fixture(args)
    validate_ruins_lever_fixture(args)
    if getattr(args, 'mantis_lever', False) and (
            not args.room_view or args.scene not in ('Fungus2_14', 'Fungus2_15', 'Fungus2_18')):
        raise ValueError('Mantis lever fixture requires an admitted Mantis room view')
    validate_stag_lever_fixture(args)
    if getattr(args,"shaman_pillar",False) and (not args.room_view or not args.stable_room or args.scene!="Crossroads_06"):
        raise ValueError("Shaman pillar fixture requires stable Crossroads_06 room view")
    validate_gate_resources_fixture(args)
    validate_lifeblood_heart_fixture(args)
    if jiji and (not args.stable_room or args.scene != 'Town'):
        raise ValueError('Jiji fixture requires stable Town room view')
    if lift_motion and (not args.room_view or not args.stable_room or args.scene != 'Ruins1_02'):
        raise ValueError('Lift-motion fixture requires stable Ruins1_02 room view')
    if args.scene not in ROOM_VIEW_SCENE_ASSETS:
        raise ValueError('Unsupported room-view scene: ' + args.scene)
    control_size = args.postprocess_controls and (args.width, args.height) == (1200, 720)
    if active_swim and args.scene != 'Crossroads_50':
        raise ValueError('Surface-swim capture requires Crossroads_50')
    if active_swim and (args.width, args.height) != (1920, 1080):
        raise ValueError('Surface-swim references must be captured at 1920x1080')
    if active_swim and args.postprocess_controls:
        raise ValueError('Surface-swim capture cannot be combined with postprocess controls')
    if not active_swim and (args.width, args.height) != (1920, 1080) and not control_size:
        raise ValueError('Room-view references must be captured at 1920x1080')
    if any((args.particles, args.effect_layers, args.effect_geometry,
            args.scene_controls, args.interaction,
            args.grass, args.ui_audit)):
        raise ValueError('Room-view mode cannot be combined with a target/effect fixture')
    gate_resources = getattr(args, 'gate_resources', False)
    hazard_point = getattr(args, 'hazard_point', None)
    hazard_return = getattr(args, 'hazard_return', None)
    if bool(hazard_point) != bool(hazard_return):
        raise ValueError('Hazard replay requires both --hazard-point and --hazard-return')
    if hazard_point:
        competing = ('stable_room', 'surface_swim', 'lift_motion', 'jiji_door', 'city_lift_toll',
                     'town_lift_lever', 'resting_stag_lever', 'shaman_pillar', 'waterways_machine', 'crossroads_switch', 'persistent_wall',
                     'false_knight_moment', 'darkness_fixture', 'postprocess_controls')
        if any(getattr(args, key, False) for key in competing):
            raise ValueError('Hazard replay requires an independent live room-view fixture')
        for value in (hazard_point, hazard_return):
            coordinates = [float(part) for part in value.split(',')]
            if len(coordinates) != 3 or not all(math.isfinite(v) for v in coordinates):
                raise ValueError('Hazard replay positions require three finite coordinates')
    original = ROOT/'_private/original/Hollow Knight'
    converted = (ROOT/'_private/converted').resolve()
    references = args.output_root or converted/'building-interiors-references'
    if not references.is_absolute():
        references = ROOT/references
    references = references.resolve()
    swim_private = (ROOT/'_private/minimum-abilities').resolve()
    allowed_output = room_view_output_allowed(references, converted, swim_private,
                                              active_swim=active_swim, laptop_root=LAPTOP_REFERENCES)
    if not allowed_output:
        raise ValueError('Room-view outputs must remain in private converted data, laptop reference frames or the surface-swim task directory')
    output = (references/args.name).resolve()
    if output == references or not output.is_relative_to(references) or output.exists():
        raise ValueError('Capture name must select a fresh room-reference directory')
    player = args.player_root or converted/'building-interiors-original'
    if not player.is_absolute():
        player = ROOT/player
    resolved_player = player.resolve()
    allowed_player = resolved_player.is_relative_to(converted) or (active_swim and resolved_player == swim_private/'source-player')
    if not allowed_player:
        raise ValueError('Room-view player copy must remain in private converted data or the surface-swim task directory')
    audit = verify(ROOT/'_private/tutorial-intro-probe/results/provenance.json', args.steam, original)
    output.mkdir(parents=True)
    (output/'source-audit.json').write_text(json.dumps(audit, indent=2)+'\n')
    level = ROOM_VIEW_SCENE_ASSETS[args.scene]
    # Boss rooms include their additive encounter scenes.
    levels = ((level, 229) if args.scene == 'Fungus3_archive_02' else
              (level, 48) if args.scene == 'Crossroads_10' else
              (level, 180) if args.scene == 'Fungus2_15' else
              (level, 355) if args.scene == 'Waterways_05' else (level,))
    if gate_resources:
        levels = tuple(dict.fromkeys((*levels, 6)))
    source_data = original/'hollow_knight_Data'
    # False Knight's level48 animation atlas is stored in sharedassets47.
    asset_levels = levels + (47,) if args.scene == 'Crossroads_10' else levels
    extra_assets = [p.name for p in source_data.iterdir()
                    if any(p.name == f'level{item}' for item in levels)
                    or any(p.name.startswith(f'sharedassets{item}.') for item in asset_levels)]
    for item in levels:
        if f'level{item}' not in extra_assets:
            raise ValueError(f'Missing saved original scene data level{item} for {args.scene}')
    # Room scenes also reference common animation/sprite banks from other levels.
    # A same-numbered bank alone leaves those libraries missing in the copy.
    extra_assets = sorted(set(extra_assets).union(
        *(scene_assets(source_data, item) for item in levels)))
    options = ['-hk3ds-surface-swim' if active_swim else '-hk3ds-room-view', '-hk3ds-scene', args.scene,
               '-hk3ds-camera', args.camera, '-hk3ds-hero', args.hero,
               '-hk3ds-frames', ','.join(map(str, args.frames)),
               '-hk3ds-seed', str(args.seed), '-hk3ds-width', str(args.width),
               '-hk3ds-height', str(args.height)]
    if greenpath_toll:
        options.append('-hk3ds-greenpath-toll-input')
    if getattr(args, 'stag_station', None):
        options += ['-hk3ds-stag-station', args.stag_station]
    if hazard_point:
        options += ['-hk3ds-hazard-point', hazard_point, '-hk3ds-hazard-return', hazard_return]
    if getattr(args, 'lifeblood_heart', False):
        options.append('-hk3ds-lifeblood-heart')
    if gate_resources:
        options.append('-hk3ds-gate-resources')
    if getattr(args, 'lantern', False):
        options.append('-hk3ds-lantern')
    if getattr(args, 'stable_room', False):
        options.append('-hk3ds-stable-room')
    if getattr(args, 'player_data', None):
        if any(not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*=-?[0-9]+', value)
               for value in args.player_data):
            raise ValueError('--player-data requires FIELD=INTEGER assignments')
        options += ['-hk3ds-player-data', ';'.join(args.player_data)]
    if getattr(args, 'lift_motion', False):
        options.append('-hk3ds-lift-motion')
    if mantis_challenge:
        options.append('-hk3ds-mantis-challenge')
    if getattr(args, 'pillar_motion', False):
        if args.scene != 'Crossroads_06' or not args.stable_room:
            raise ValueError('--pillar-motion requires Crossroads_06 --room-view --stable-room')
        options.append('-hk3ds-pillar-motion')
    if jiji:
        options += ['-hk3ds-jiji-door', jiji]
    if town_lever:
        options.append('-hk3ds-town-lift-lever')
    if getattr(args, 'ruins_lever', False):
        options.append('-hk3ds-ruins-lever')
    if getattr(args, 'mantis_lever', False):
        options.append('-hk3ds-mantis-lever')
    if getattr(args, 'resting_stag_lever', False):
        options.append('-hk3ds-resting-stag-lever')
    if getattr(args,'shaman_pillar',False):
        options.append('-hk3ds-shaman-pillar')
    if city_toll:
        options.append('-hk3ds-city-lift-toll')
    if city_crest:
        options += ['-hk3ds-city-crest-moment', city_crest]
    if waterways:
        options.append('-hk3ds-waterways-machine')
    if crossroads_switch:
        options.append('-hk3ds-crossroads-switch')
    if getattr(args, 'room_borders', False):
        options.append('-hk3ds-room-borders')
    if args.scene == 'Crossroads_10':
        options.append('-hk3ds-cross-false-knight-trigger')
    moment = getattr(args, 'false_knight_moment', None)
    if moment:
        if args.scene != 'Crossroads_10':
            raise ValueError('--false-knight-moment requires Crossroads_10 --room-view')
        options += ['-hk3ds-false-knight-moment', moment]
    if args.postprocess_controls:
        options.append('-hk3ds-postprocess-controls')
    frame_definition = ('0=first end-of-render after live source swimming and 60 settling frames; '
                        'no FSM state injection; offsets at controlled 60 Hz' if active_swim else
                        '0=first end-of-render after 3-second scene settle; no Hit or state injection; offsets at controlled 60 Hz')
    if hazard_point:
        frame_definition = '0=live source hazard return after 240 controlled frames; position-only teleport; no damage or FSM injection'
    if getattr(args, 'lifeblood_heart', False):
        frame_definition += '; prepared charm8 flags, source CharmUpdate computes blue health; static equipment effect only'
    if gate_resources:
        frame_definition = 'Health2/Soul44/reserve0 seeded once; actual Town left1 trigger enters Tutorial_01/right1; 0=180 live frames after load'
    if lift_motion:
        frame_definition = ('Controlled source Lift Control events: CONTROL DOWN at offset 0, '
                            'CONTROL UP at 120; offsets at controlled 60 Hz; visual fixture only')
    if mantis_challenge:
        frame_definition = ('Challenge Prompt Can Talk? state injection after source grounded/CanTalk and trigger checks; '
                            'invincible live hero; offsets at controlled 60 Hz; no health/progression writes')
    if getattr(args, 'pillar_motion', False):
        frame_definition = 'Source Gate Switch NAIL HIT at offset 1; offsets at controlled 60 Hz; visual fixture only'
    if town_lever:
        frame_definition = ('Town mineLiftOpened=False before scene load; source Switch Control Hit state '
                            'at offset 0, bypassing nail detection; offsets at controlled 60 Hz; visual fixture only')
    if getattr(args, 'resting_stag_lever', False):
        frame_definition = ('RestingGrounds_09 source Switch Control Hit at offset 0, bypassing nail detection; '
                            'source OPEN/reveal/persistence remain live; controlled 60 Hz; visual fixture only')
    if getattr(args,"shaman_pillar",False):
        frame_definition = "Crossroads_06 source NAIL HIT event at offset0; live source FSM/collision/persistence; visual fixture only"
    if crossroads_switch:
        frame_definition = ('Crossroads_03 source toll switch Hit state at offset 0, bypassing nail detection; '
                            'offsets at controlled 60 Hz; visual fixture only')
    if city_crest:
        frame_definition = ('City Crest source Open FSM ' + ('Open 1' if city_crest == 'opening' else 'Slam') +
                            ' at offset0; bypasses conversation/hero trigger only; source tweens and dust run at60Hz; visual fixture only')
    if city_toll:
        frame_definition = ('City toll: geo=200, cityLift1=False before load; Take Control then Box Up at offset0 bypasses approach motion only; '
                            'global Text YN End Conversation/YES at240 bypasses choice input only; source HideText/price/debit/unlock run live. Visual reference only.')
    if greenpath_toll:
        frame_definition = 'Live Greenpath source hero; up at frame 30, jump at 210; no FSM injection; require 50 geo debit.'
    if jiji:
        frame_definition = 'Source Jiji CONVO START after room settle; keys seeded before scene load; 150 live frames before offset zero; isolated UI reference'
    if waterways:
        frame_definition = ('Ruins1_05b simpleKeys=1 and openedWaterwaysManhole=False before load; '
                            'source Conversation Control Activate at offset 0, bypassing dialogue only; '
                            'controlled 60 Hz; visual fixture only, not gameplay acceptance')
    if moment == 'fight-start':
        frame_definition = '0=source FalseyControl State 1, first grounded intro Idle; no damage or state injection; offsets at controlled 60 Hz'
    elif moment:
        frame_definition = ('0=source ' + moment + '; controlled HealthManager Pure Nail damage21 '
                            'with invulnerable hero; source FSMs/physics run normally; visual fixture only')
    status = dict(build=audit['build'], complete=False, configuration=vars(args).copy(),
                  frame_definition=frame_definition)
    try:
        lock = player.with_suffix('.lock')
        lock.parent.mkdir(parents=True, exist_ok=True)
        with lock.open('x'):
            pass
        try:
            run(original, player, args.dotnet, ROOT/'tools/unity_frame_capture_probe.cs.txt', True,
                ROOT/'tools/unity_frame_capture_patch.cs.txt', options, timeout=3600,
                graphics=True, extra_assets=extra_assets, converted_inputs=False)
        finally:
            lock.unlink()
            drop_player_copy(player)
        results = player/'results'
        if greenpath_toll:
            validate_greenpath_toll_trace(results/'greenpath-toll-input.csv')
        vignette_scale = getattr(args, 'expected_vignette_scale', None)
        if vignette_scale is not None:
            for offset in args.frames:
                name = f'frame-{offset:03}'
                state = json.loads((results/f'lighting-{name}.json').read_text())
                validate_vignette_scale(state['vignette_scale'], name, vignette_scale)
        if args.postprocess_controls:
            validate_postprocess_controls(results, args.frames, (args.width, args.height), include_before=False)
            shutil.copyfile(results/'postprocess-controls.jsonl', output/'postprocess-controls.jsonl')
        if jiji:
            shutil.copyfile(results/'jiji-source-fsms.txt', output/'jiji-source-fsms.txt')
        if hazard_point:
            validate_hazard_trace(results/'hazard-sequence.csv', [float(v) for v in hazard_return.split(',')])
        if getattr(args, 'lifeblood_heart', False):
            shutil.copyfile(results/'lifeblood-heart.csv', output/'lifeblood-heart.csv')
        if gate_resources:
            validate_gate_resource_trace(results/'gate-resources.csv')
            shutil.copyfile(results/'gate-resources.csv', output/'gate-resources.csv')
        if mantis_challenge:
            for evidence in ('mantis-injection.txt', 'mantis-initialization.csv', 'mantis-challenge.csv', 'mantis-source-fsms.txt'):
                shutil.copyfile(results/evidence, output/evidence)
        if active_swim:
            # Required evidence that the real controller and water FSM stayed live.
            shutil.copyfile(results/'surface-swim.csv', output/'surface-swim.csv')
        for offset in args.frames:
            name = f'frame-{offset:03}'
            source = results/f'{name}-full.png'
            with Image.open(source) as image:
                if image.size != (args.width, args.height) or not image.getbbox():
                    raise ValueError('Invalid full-size room capture: '+name)
                shutil.copyfile(source, output/source.name)
                image.resize((400, 240), Image.Resampling.LANCZOS).save(output/(name+'.png'))
            shutil.copyfile(results/f'lighting-{name}.json', output/f'lighting-{name}.json')
            if args.postprocess_controls:
                shutil.copyfile(results/f'control-{name}-state-before.json', output/f'control-{name}-state-before.json')
                shutil.copyfile(results/f'control-{name}-room-renderers.json', output/f'control-{name}-room-renderers.json')
                for variant in POSTPROCESS_VARIANTS:
                    source = results/f'control-{name}-{variant}-full.png'
                    shutil.copyfile(source, output/source.name)
                    with Image.open(source) as image:
                        image.resize((400, 240), Image.Resampling.LANCZOS).save(output/f'control-{name}-{variant}.png')
        for name in ('greenpath-toll-input.csv', 'frames.csv', 'hazard-sequence.csv', 'hazard-initialization.csv', 'lift-motion.csv', 'pillar-motion.csv', 'town-lever.csv', 'city-crest.csv', 'resting-stag-lever.csv', 'shaman-pillar.csv', 'waterways-machine.csv',
                     'crossroads-switch.csv', 'provenance.json', 'observer-preservation.json',
                     'complete.txt', 'false-knight-states.txt', 'false-knight-moment.json'):
            if (results/name).exists():
                shutil.copyfile(results/name, output/('probe-'+name))
        if getattr(args, 'room_borders', False):
            shutil.copyfile(results/'runtime-borders.jsonl', output/'runtime-borders.jsonl')
        if getattr(args, 'stable_room', False):
            state = results/'stable-room-state.json'
            if not state.exists():
                raise ValueError('Stable room capture is missing state evidence')
            validate_stable_room_state(state)
            shutil.copyfile(state, output/'stable-room-state.json')
            shutil.copyfile(results/'stable-room-timing.json', output/'stable-room-timing.json')
        if moment:
            for offset in args.frames:
                for prefix in ('knight-frame', 'knight-renderers', 'knight-particles'):
                    source = results/f'{prefix}-{offset:03}.json'
                    if source.exists():
                        shutil.copyfile(source, output/source.name)
        status['complete'] = True
    finally:
        status['files'] = {p.name: digest(p) for p in output.iterdir() if p.is_file()}
        (output/'provenance.json').write_text(json.dumps(status, indent=2, default=str)+'\n')
    return output


def drop_player_copy(player):
    """Remove the game copy a capture run staged; keep results/.

    probe_unity_physics.run copies the selected game data, MonoBleedingEdge and a patched Managed
    afresh on every run (no reuse, no hard links), so the copy is dead weight once the run exits.
    2026-11-01: per-run player roots left ~800 MB each; one laptop task held 10.6 GB and the drive
    reached 0 GB free."""
    for name in ('hollow_knight_Data', 'MonoBleedingEdge', 'patcher'):
        shutil.rmtree(player / name, ignore_errors=True)
    for path in player.glob('*'):
        if path.is_file() and path.suffix.lower() in ('.exe', '.dll'):
            path.unlink(missing_ok=True)


def validate_cast_frames(results, frames, interaction):
    """Require the source FSM fixture to display the requested authored cast clip."""
    expected = {'cast1': 'Fireball1 Cast', 'cast2': 'Fireball2 Cast'}[interaction]
    with (results/'cast-frames.csv').open() as stream:
        rows = list(csv.DictReader(stream))
    if [int(row['offset']) for row in rows] != frames:
        raise ValueError('Incomplete source cast frame ledger')
    if any(row['clip'] != expected or not 0 <= int(row['clipFrame']) < 6
           or float(row['clipFps']) != 20 for row in rows):
        raise ValueError('Source cast fixture did not retain requested six-frame 20 fps clip')
    return rows


def capture(args):
    validate_town_lift_fixture(args)
    if getattr(args, 'uumuu_blood_burst', False) and (
            not getattr(args, 'enemy_death', False) or args.scene != 'Fungus3_archive_02'):
        raise ValueError('--uumuu-blood-burst requires --enemy-death in Fungus3_archive_02')
    if getattr(args, 'room_view', False) or getattr(args, 'surface_swim', False):
        return capture_room_view(args)
    if getattr(args, 'ui_audit', None):
        from capture_ui_audit import capture_ui
        return capture_ui(args)
    postprocess_controls = getattr(args, 'postprocess_controls', False)
    if postprocess_controls and (args.effect_layers or args.effect_geometry):
        raise ValueError('--postprocess-controls cannot be combined with --effect-layers/--effect-geometry')
    references = (getattr(args, 'output_root', None) or REFERENCES).resolve()
    allowed_reference_roots = ((ROOT/'_private/converted').resolve(),
                               (ROOT/'_private/laptop-reference-frames').resolve(),
                               Path('C:/Projects/HK3DS/_private/laptop-reference-frames').resolve())
    if references != REFERENCES.resolve() and not any(references.is_relative_to(root) for root in allowed_reference_roots):
        raise ValueError('Alternate reference output must remain in private converted data or laptop-reference-frames')
    output = (references / args.name).resolve()
    if output == references or not output.is_relative_to(references) or output.exists():
        raise ValueError('Capture name must select a fresh directory within reference-frames')
    player = ROOT / ('_private/converted/geo-rock-frame-capture-probe' if getattr(args, 'geo_rock', None) else
                     '_private/stalactite-frame-capture-probe' if getattr(args, 'stalactite', False) else '_private/frame-capture-probe')
    if getattr(args, 'enemy_death', False):
        player = ROOT / '_private/converted/enemy-death-frame-capture-probe'
    if getattr(args, 'player_root', None):
        player = args.player_root.resolve()
        if not player.is_relative_to((ROOT/'_private/converted').resolve()):
            raise ValueError('Alternate reference player must remain in private converted data')
    original = ROOT / '_private/original/Hollow Knight'
    if getattr(args, 'interaction', None) and not getattr(args, 'player_root', None):
        player = ROOT / '_private/converted/interaction-frame-capture-probe'
    if getattr(args, 'grass', False):
        player = ROOT / '_private/converted/grass-frame-capture-probe'
    try:
        audit = verify(ROOT / '_private/tutorial-intro-probe/results/provenance.json',
                       args.steam, original)
    except FileNotFoundError:
        manifest = args.steam.resolve().parents[1] / 'appmanifest_367520.acf'
        if manifest.exists():
            raise
        audit = verify_archived_source(ROOT / '_private/tutorial-intro-probe/results/provenance.json', original)
    output.mkdir(parents=True)
    (output / 'source-audit.json').write_text(json.dumps(audit, indent=2) + '\n')
    manifest = json.loads((ROOT / '_private/research/decor-effects/decor-effects.json').read_text())
    if args.particles:
        record = next(r for r in manifest['derived']['records'] if r['owner'] == args.owner)
        source = ROOT / '_private/research/decor-effects'
        owner_name = json.loads((source / f'level6-{args.owner}.json').read_text())['m_Name']
        if owner_name != args.target:
            raise ValueError('Particle owner does not match target')
        part_names = [json.loads((source / name).read_text())['m_Name'] for name in record['debris']]
    # The legacy Tutorial fixture already copies level6 and its dependencies.
    selected_assets = []
    if args.scene != 'Tutorial_01':
        scene_index = SCENES.get(args.scene, getattr(args, 'scene_level', None) or ROOM_VIEW_SCENE_ASSETS.get(args.scene))
        if scene_index is None:
            raise ValueError(f'No original scene bundle mapping for {args.scene}')
        scene_map = ROOT / '_private/research/crossroads-scenes' / args.scene / 'visuals/room-visuals-map.json'
        if not scene_map.is_file():
            raise FileNotFoundError(f'Missing source asset manifest for {args.scene}: {scene_map}')
        selected_assets = selected_scene_assets(original / 'hollow_knight_Data', scene_index,
                                                json.loads(scene_map.read_text())['source_sha256'])
    # Check the authored scene/assemblies used by the decor conversion as well.
    expected = {original / n: h for n, h in manifest['input_sha256'].items()}
    dependency_assets = scene_assets(original / 'hollow_knight_Data', getattr(args, 'scene_level', None))
    # The Archives arena is level228; Uumuu is an additive level229 overlay.
    if args.scene == 'Fungus3_archive_02':
        dependency_assets = tuple(dict.fromkeys((*dependency_assets,
            *scene_assets(original / 'hollow_knight_Data', 229))))
    if args.scene == 'Fungus1_04':
        dependency_assets = tuple(dict.fromkeys((*dependency_assets, *scene_assets(original / 'hollow_knight_Data', 133))))
    for name in dependency_assets:
        path = original / 'hollow_knight_Data' / name
        expected.setdefault(path, digest(path))
    extra_assets = tuple(dict.fromkeys(selected_assets + list(dependency_assets)))
    for path, sha in expected.items():
        if digest(path) != sha:
            raise ValueError('Archived decor source changed: ' + str(path))
    options = ['-hk3ds-scene', args.scene, '-hk3ds-target', args.target,
               '-hk3ds-camera', args.camera, '-hk3ds-hero', args.hero,
               '-hk3ds-frames', ','.join(map(str, args.frames)), '-hk3ds-seed', str(args.seed),
               '-hk3ds-width', str(args.width), '-hk3ds-height', str(args.height)]
    if getattr(args, 'lantern', False):
        options.append('-hk3ds-lantern')
    if getattr(args, 'player_data', None):
        options += ['-hk3ds-player-data', ';'.join(args.player_data)]
    if getattr(args, 'embedded_stalactite', False):
        options += ['-hk3ds-embedded-stalactite']
    if getattr(args, 'spell_hit', False):
        options += ['-hk3ds-spell-hit']
    if getattr(args, 'stalactite', False):
        options += ['-hk3ds-stalactite']
    if getattr(args, 'collapse', False):
        options += ['-hk3ds-collapse']
    if getattr(args, 'persistent_wall', False):
        options += ['-hk3ds-persistent-wall']
    if getattr(args, 'geo_rock', None):
        options += ['-hk3ds-geo-rock', args.geo_rock]
    if getattr(args, 'enemy_death', False):
        options += ['-hk3ds-enemy-death']
    if getattr(args, 'uumuu_blood_burst', False):
        options += ['-hk3ds-uumuu-blood-burst']
    if args.particles:
        options += ['-hk3ds-particles', '-hk3ds-owner', str(args.owner)]
    if args.effect_layers:
        options += ['-hk3ds-effect-layers']
    if postprocess_controls:
        options += ['-hk3ds-postprocess-controls']
    if getattr(args, 'scene_controls', False):
        options += ['-hk3ds-scene-controls']
    if getattr(args, 'interaction', None):
        options += ['-hk3ds-interaction',args.interaction]
    if getattr(args, 'grass', False):
        options += ['-hk3ds-grass']
    if getattr(args, 'observe', False):
        options += ['-hk3ds-observe']
    if getattr(args, 'hornet_moment', None):
        if args.scene != 'Fungus1_04' or args.target != 'Hornet Boss 1' or not args.observe:
            raise ValueError('--hornet-moment requires Fungus1_04 / Hornet Boss 1 / --observe')
        options += ['-hk3ds-hornet-moment', args.hornet_moment]
    if getattr(args, 'cloak_pickup', False):
        if args.scene != 'Fungus1_04' or args.target != 'Shiny Item' or not args.observe:
            raise ValueError('--cloak-pickup requires Fungus1_04 / Shiny Item / --observe')
        options += ['-hk3ds-cloak-pickup']
    if getattr(args, 'bretta_first_dialogue', False):
        options += ['-hk3ds-bretta-first-dialogue']
    if getattr(args, 'sly_first_dialogue', False):
        options += ['-hk3ds-sly-first-dialogue']
    if getattr(args, 'sly_awake', False):
        options += ['-hk3ds-sly-awake']
    if getattr(args, 'sly_title', False):
        options += ['-hk3ds-sly-title']
    status = dict(build=audit['build'], complete=False, configuration=vars(args).copy(),
                  frame_definition='0=end-of-render of ' + ('controlled source Spell Control Fireball state injection (visual-only; frozen hero, no gameplay timing claim)' if getattr(args, 'interaction', None) in ('cast1', 'cast2') else 'ordinary Crawler HealthManager lethal Nail hit and authored Geo drop' if getattr(args, 'enemy_death', False) else 'ambient grass observation' if getattr(args, 'grass', False) else 'source sprite observation' if getattr(args, 'observe', False) else 'embedded stalactite Hit after source landing' if getattr(args, 'embedded_stalactite', False) else 'Persistent wall controlled Break state' if getattr(args, 'persistent_wall', False) else 'Collapser controlled Break state' if getattr(args, 'collapse', False) else 'Geo Rock controlled Hit state' if getattr(args, 'geo_rock', None) else 'Fall start' if getattr(args, 'stalactite', False) else 'Hit') + ' frame; offsets at controlled 60 Hz',
                  source_sha256={str(p.relative_to(ROOT)): h for p, h in expected.items()})
    try:
        lock = player.with_suffix('.lock') if getattr(args, 'player_root', None) else ROOT / '_private/frame-capture-probe.lock'
        with lock.open('x'):
            pass
        try:
            probe = 'unity_decor_layers_probe.cs.txt' if args.effect_layers else 'unity_frame_capture_probe.cs.txt'
            patch = 'unity_decor_layers_patch.cs.txt' if args.effect_layers else 'unity_frame_capture_patch.cs.txt'
            if getattr(args, 'scene_controls', False):
                patch = 'unity_decor_layers_patch.cs.txt'
            if args.effect_geometry:
                probe = 'unity_decor_geometry_probe.cs.txt'
            capture_assets = set(extra_assets)
            if args.scene == 'Room_ruinhouse':
                data = original / 'hollow_knight_Data'
                capture_assets.update(room_ruinhouse_extra_assets(data))
            run(original, player, args.dotnet, ROOT / 'tools' / probe, True,
                ROOT / 'tools' / patch, options, timeout=180, graphics=True,
                extra_assets=tuple(sorted(capture_assets)))
            rows = validate_frames(player / 'results', args.frames, (args.width, args.height))
            fixture_hero = ([float(v) for v in args.hero.split(',')] if getattr(args, 'hornet_moment', None) != 'sphere-hit' else
                            json.loads((player / 'results/hornet-fixture.json').read_text())['heroPosition'])
            validate_lighting(player / 'results', args.frames,
                              fixture_hero,
                              [float(v) for v in args.camera.split(',')],
                              getattr(args, 'expected_vignette_scale', None))
            if getattr(args, 'hornet_moment', None) == 'sphere-hit':
                validate_sphere_hit(player / 'results')
            if getattr(args, 'interaction', None) in ('cast1', 'cast2'):
                validate_cast_frames(player / 'results', args.frames, args.interaction)
            if getattr(args, 'grass', False):
                validate_grass(player / 'results', args.frames, args.target)
            if args.particles:
                validate_snapshots(player / 'results/particles.jsonl', args.frames,
                                   args.owner, args.target, part_names, rows)
            if args.effect_layers:
                validate_observations(player / 'results', args.frames, args.owner, args.target, rows)
            if postprocess_controls:
                validate_postprocess_controls(player / 'results', args.frames, (args.width, args.height))
            if getattr(args, 'scene_controls', False):
                validate_scene_controls(player / 'results', args.frames, (args.width, args.height))
        finally:
            lock.unlink()
            drop_player_copy(player)
        for name in ['before'] + ['frame-' + format(int(row['offset']), '03') for row in rows]:
            if getattr(args, 'grass', False):
                shutil.copyfile(player / f'results/grass-{name}.json', output / f'grass-{name}.json')
            shutil.copyfile(player / f'results/lighting-{name}.json', output / f'lighting-{name}.json')
            spatter = player / f'results/spatter-{name}.json'
            if spatter.exists():
                shutil.copyfile(spatter, output / spatter.name)
            source = player / ('results/' + name + '-full.png')
            shutil.copyfile(source, output / source.name)
            with Image.open(source) as im:
                if im.size != (args.width, args.height) or not im.getbbox():
                    raise ValueError('Invalid full size capture')
                im.resize((400, 240), Image.Resampling.LANCZOS).save(output / (name + '.png'))
            if postprocess_controls:
                for variant in POSTPROCESS_VARIANTS + (SCENE_VARIANTS if getattr(args, 'scene_controls', False) else ()):
                    control = f'control-{name}-{variant}'
                    source = player / ('results/' + control + '-full.png')
                    shutil.copyfile(source, output / source.name)
                    with Image.open(source) as im:
                        im.resize((400, 240), Image.Resampling.LANCZOS).save(output / (control + '.png'))
        if postprocess_controls:
            shutil.copyfile(player / 'results/postprocess-controls.jsonl', output / 'postprocess-controls.jsonl')
        if getattr(args, 'scene_controls', False):
            for source in (player / 'results').glob('control-*-scene-renderers.json'):
                shutil.copyfile(source, output / source.name)
        for name in ('frames.csv', 'provenance.json', 'observer-preservation.json'):
            shutil.copyfile(player / 'results' / name, output / ('probe-' + name))
        if getattr(args, 'hornet_moment', None):
            shutil.copyfile(player / 'results/hornet-fixture.json', output / 'hornet-fixture.json')
        if getattr(args, 'cloak_pickup', False):
            shutil.copyfile(player / 'results/cloak-fixture.json', output / 'cloak-fixture.json')
        if getattr(args, 'persistent_wall', False):
            shutil.copyfile(player / 'results/wall-state.jsonl', output / 'probe-wall-state.jsonl')
        if getattr(args, 'bretta_first_dialogue', False):
            shutil.copyfile(player / 'results/bretta-first-dialogue.json', output / 'bretta-first-dialogue.json')
            validate_bretta_first_dialogue_evidence(json.loads((output / 'bretta-first-dialogue.json').read_text()))
        if getattr(args, 'sly_first_dialogue', False):
            shutil.copyfile(player / 'results/sly-first-dialogue.json',
                            output / 'sly-first-dialogue.json')
            evidence = json.loads((output / 'sly-first-dialogue.json').read_text())
            validate_sly_first_dialogue_language(evidence.get('language'))
        if getattr(args, 'sly_awake', False):
            shutil.copyfile(player / 'results/sly-awake.json', output / 'sly-awake.json')
            evidence = json.loads((output / 'sly-awake.json').read_text())
            validate_sly_awake_evidence(evidence)
        if getattr(args, 'sly_title', False):
            shutil.copyfile(player / 'results/sly-title.json', output / 'sly-title.json')
            evidence = json.loads((output / 'sly-title.json').read_text())
            validate_sly_title_evidence(evidence)
        if getattr(args, 'interaction', None) in ('cast1', 'cast2'):
            shutil.copyfile(player / 'results/cast-frames.csv', output / 'cast-frames.csv')
            shutil.copyfile(player / 'results/cast-fixture.json', output / 'cast-fixture.json')
        if getattr(args, 'enemy_death', False):
            shutil.copyfile(player / 'results/enemy-death-state.json', output / 'enemy-death-state.json')
            shutil.copyfile(player / 'results/enemy-death-geo-frames.jsonl', output / 'enemy-death-geo-frames.jsonl')
        if args.particles:
            shutil.copyfile(player / 'results/particles.jsonl', output / 'particles.jsonl')
        if args.effect_layers:
            for source in [*(player / 'results').glob('layer-*-full.png'),
                           player / 'results/effect-particles.jsonl', player / 'results/effect-objects.jsonl',
                           player / 'results/effect-cameras.json']:
                shutil.copyfile(source, output / source.name)
        for name in ('bootstrap-errors.txt','shutdown-errors.txt'):
            if (player / 'results' / name).exists():
                shutil.copyfile(player / 'results' / name, output / ('probe-' + name))
        status['complete'] = True
    finally:
        status['original_inputs_unchanged'] = all(digest(p) == h for p, h in expected.items())
        status['files'] = {p.name: digest(p) for p in output.iterdir() if p.is_file()}
        (output / 'provenance.json').write_text(json.dumps(status, indent=2, default=str) + '\n')
    if not status['original_inputs_unchanged']:
        raise ValueError('Original source changed')
    return output


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('name')
    p.add_argument('--scene', default='Tutorial_01')
    p.add_argument('--archive-provenance', action='store_true', help='Verify the archived original against saved source hashes instead of a Steam installation')
    p.add_argument('--lantern', action='store_true', help='Inject owned Lantern before loading the reference room')
    p.add_argument('--scene-level', type=int, help='Source level file number for scenes outside the default Tutorial fixture')
    p.add_argument('--target', help='Unique object name, or full hierarchy for duplicate names (basic capture)')
    p.add_argument('--camera', required=True, help='World x,y,z')
    p.add_argument('--hero', required=True, help='World x,y,z')
    p.add_argument('--frames', type=frames_argument, default=frames_argument('0,2,5,10,20,40'))
    p.add_argument('--seed', type=int, default=1729)
    p.add_argument('--spell-hit', action='store_true', help='Deliver an ordinary source Spell hit to the selected breakable')
    p.add_argument('--player-data', action='append', default=[], metavar='FIELD=INTEGER',
                   help='Seed original bool/int PlayerData before loading the room; recorded in capture arguments')
    p.add_argument('--expected-vignette-scale', type=float,
                   help='Reject reference frames with a different settled vignette scale')
    p.add_argument('--stalactite', action='store_true', help='Start the source StalactiteControl.Fall coroutine instead of a Breakable hit')
    p.add_argument('--embedded-stalactite', action='store_true', help='Wait for source Fall landing, then hit its embedded Breakable')
    p.add_argument('--collapse', action='store_true', help='Capture the source Collapser Break state with physics and lighting active')
    p.add_argument('--persistent-wall', action='store_true', help='Capture breakable_wall_v2/break_floor Break with original masks, particles and lighting active')
    p.add_argument('--grass', action='store_true', help='Observe ambient GrassBehaviour motion without a hit; save native shader/renderer state')
    p.add_argument('--observe', action='store_true', help='Observe a tk2dSprite target without invoking Hit; keep its source FSM and animation running')
    p.add_argument('--hornet-moment', choices=('fight', 'dash', 'needle', 'sphere', 'sphere-hit', 'defeat'),
                   help='Complete the source first-meeting dialogue through page input, then observe a live Protector state; defeat applies one lethal nail hit')
    p.add_argument('--cloak-pickup', action='store_true',
                   help='Send the source Shiny Control START INSPECT event and observe the Mothwing Cloak item message; seed hornet1Defeated=1 explicitly')
    p.add_argument('--bretta-first-dialogue', action='store_true', help='Observe Bretta first dialogue through the authored NPC input event')
    p.add_argument('--sly-first-dialogue', action='store_true', help='Cross the source Sly trigger and send UP PRESSED through npc_control while Conversation Control is Idle')
    p.add_argument('--sly-awake', action='store_true', help='Interact through npc_control and complete the first source dialogue to wake Sly')
    p.add_argument('--sly-title', action='store_true', help='Complete the authored Sly rescue conversations and capture the source SLY title through its FSM')
    p.add_argument('--stag-station', choices=('arrival','dialogue','menu','toll'), help='Source Stag presentation fixture (room view)')
    p.add_argument('--room-view', action='store_true', help='Capture a settled original room view without injecting a target event')
    p.add_argument('--mantis-challenge', action='store_true', help='Capture the live Mantis fight from the guarded source Challenge Prompt Can Talk? state; records state injection')
    p.add_argument('--false-knight-moment', choices=('fight-start', 'armour-break', 'victory'),
                   help='Room-view visual fixture: source grounded intro, first Stun Open or defeat flag; damage21 only after the intro')
    p.add_argument('--lifeblood-heart', action='store_true', help='Prepare charm8 and let source CharmUpdate calculate its blue masks; stable Tutorial_01 only')
    p.add_argument('--gate-resources', action='store_true', help='Seed resources once and cross the actual Town left1 gate into Tutorial_01')
    p.add_argument('--hazard-point', help='Live source hazard contact x,y,z; requires --room-view and --hazard-return')
    p.add_argument('--hazard-return', help='Expected settled live hero x,y,z after 240 source frames')
    p.add_argument('--room-borders', action='store_true', help='Record runtime SceneBorder geometry during a room-view capture')
    p.add_argument('--stable-room', action='store_true', help='Use the stable reference room set and deterministic idle state (requires --room-view)')
    p.add_argument('--jiji-door', choices=('no-key', 'key'), help='Town source CONVO START after settling; records live choice FSM actions')
    p.add_argument('--city-crest-moment', choices=('opening', 'slam'), help='Source City gate Open1/Slam visual fixture, bypassing dialogue/hero trigger only')
    p.add_argument('--city-lift-toll',action='store_true',help='Source City toll Box Up and global YN Take Geo visual fixture; records actual debit/unlock')
    p.add_argument('--greenpath-toll-input', action='store_true', help='Drive live Greenpath toll with ordinary up and jump input; seed geo with --player-data')
    p.add_argument('--town-lift-lever', action='store_true', help='Town source Switch Control Hit at offset 0; bypasses hit detection, visual fixture only')
    p.add_argument('--ruins-lever', action='store_true', help='Ruins1_05/05b/05c source lever Hit at offset0; bypasses nail detection, visual fixture only')
    p.add_argument('--mantis-lever', action='store_true', help='Fungus2_14/15/18 source Mantis Lever Hit at offset0; bypasses nail detection, visual fixture only')
    p.add_argument('--shaman-pillar',action='store_true',help='Crossroads_06 source NAIL HIT after settling; visual fixture only')
    p.add_argument('--resting-stag-lever', action='store_true', help='RestingGrounds_09 source Switch Control Hit at offset 0; bypasses hit detection, visual fixture only')
    p.add_argument('--waterways-machine', action='store_true', help='Ruins1_05b source Conversation Control Activate at offset 0; visual fixture only')
    p.add_argument('--crossroads-switch', action='store_true', help='Crossroads_03 source toll switch Hit at offset 0; visual fixture only')
    p.add_argument('--lift-motion', action='store_true', help='Ruins1_02 source lift CONTROL DOWN at offset 0, CONTROL UP at 120; visual fixture only')
    p.add_argument('--pillar-motion', action='store_true', help='Crossroads_06 source Gate Switch NAIL HIT at offset 1; visual fixture only')
    p.add_argument('--surface-swim', action='store_true', help='Drop the live original hero into Blue Lake and capture only after its Surface Water FSM enters a swimming state')
    p.add_argument('--interaction', choices=('tink','prompt','heal','cast1','cast2','mantis-switch','bounce-shroom'), help='Source interaction presentation fixture; physics and image effects remain active')
    p.add_argument('--geo-rock', choices=('hit', 'break'), help='Controlled source Geo Rock Hit state; seeds Hits and Attack Direction, bypassing event-sender lookup')
    p.add_argument('--enemy-death', action='store_true', help='Kill the selected ordinary HealthManager through source Nail damage and capture its physical Geo drop')
    p.add_argument('--uumuu-blood-burst', action='store_true', help='Isolate the source corpse Blow SpawnBlood action after death; hide corpse and pause its Death FSM (visual fixture only)')
    p.add_argument('--output-root', type=Path, help='Alternate private converted directory for reference captures')
    p.add_argument('--player-root', type=Path, help='Isolated player copy inside private converted data for worktree captures')
    p.add_argument('--particles', action='store_true', help='Snapshot only authored debris systems')
    p.add_argument('--effect-layers', action='store_true', help='Observe owner Hit Spawn returns and same-frame render contributions')
    p.add_argument('--effect-geometry', action='store_true', help='Use a separate observer with flash sprite, animation, color and TRS state')
    p.add_argument('--postprocess-controls', action='store_true', help='Render same-state bloom/curve controls and record live camera/lighting metadata')
    p.add_argument('--scene-controls', action='store_true', help='Observe Hit Spawn returns and exclude owner dust/flash/debris from raw scene controls; requires --postprocess-controls --particles')
    p.add_argument('--owner', type=int, help='Exported level6 owner path ID (required with --particles)')
    p.add_argument('--width', type=int)
    p.add_argument('--height', type=int)
    p.add_argument('--ui-audit', choices=('hud', 'panels', 'opening', 'blanker', 'inspect', 'soul-limiter', 'menus'),
                   help='Original UI presentation fixture, isolated from the port runtime; use 1920x1080')
    p.add_argument('--dotnet', type=Path, default=Path('C:/Program Files/dotnet'))
    p.add_argument('--steam', type=Path, default=Path('C:/Program Files (x86)/Steam/steamapps/common/Hollow Knight'))
    args = p.parse_args()
    if args.width is None:
        args.width = 1920 if args.room_view or args.surface_swim else 1200
    if args.height is None:
        args.height = 1080 if args.room_view or args.surface_swim else 720
    if not args.room_view and not args.surface_swim and not args.target:
        p.error('--target is required unless --room-view is selected')
    if args.room_view and args.surface_swim:
        p.error('--room-view and --surface-swim are separate fixtures')
    if args.room_view and (args.effect_layers or args.effect_geometry or args.scene_controls):
        p.error('--room-view cannot be combined with effect/control capture modes')
    if args.bretta_first_dialogue:
        try:
            validate_bretta_first_dialogue(args.scene, args.target, args.observe)
        except ValueError as error:
            p.error(str(error))
    if args.sly_first_dialogue:
        try:
            validate_sly_first_dialogue(args.scene, args.target, args.observe)
        except ValueError as error:
            p.error(str(error))
    if args.sly_awake:
        try:
            validate_sly_awake(args.scene, args.target, args.observe)
        except ValueError as error:
            p.error(str(error))
    if args.sly_title:
        try:
            validate_sly_title(args.scene, args.target, args.observe)
        except ValueError as error:
            p.error(str(error))
    if sum((args.bretta_first_dialogue, args.sly_first_dialogue, args.sly_awake, args.sly_title)) > 1:
        p.error('--sly-first-dialogue, --sly-awake and --sly-title are mutually exclusive')
    if args.observe and any((args.stalactite, args.embedded_stalactite, args.collapse, args.grass,
                             args.interaction, args.geo_rock, args.particles, args.effect_layers,
                             args.effect_geometry, args.postprocess_controls, args.scene_controls,
                             args.ui_audit)):
        p.error('--observe requires an independent source sprite fixture')
    if (args.hazard_point or args.hazard_return) and not args.room_view:
        p.error('Hazard replay requires --room-view')
    if args.stable_room and not args.room_view:
        p.error('--stable-room requires --room-view')
    if args.lift_motion and (not args.room_view or not args.stable_room or args.scene != 'Ruins1_02'):
        p.error('--lift-motion requires --room-view --stable-room --scene Ruins1_02')
    if args.room_borders and not args.room_view:
        p.error('--room-borders requires --room-view')
    if args.grass and any((args.stalactite, args.interaction, args.geo_rock, args.enemy_death, args.particles,
                           args.effect_layers, args.effect_geometry, args.postprocess_controls,
                           args.scene_controls, args.ui_audit)):
        p.error('--grass requires an independent ambient fixture')
    if args.scene_controls and not (args.postprocess_controls and args.particles):
        p.error('--scene-controls requires --postprocess-controls --particles')
    if args.postprocess_controls and (args.effect_layers or args.effect_geometry):
        p.error('--postprocess-controls cannot be combined with --effect-layers/--effect-geometry')
    if args.particles and args.owner is None:
        p.error('--particles requires --owner')
    if args.effect_layers and not args.particles:
        p.error('--effect-layers requires --particles')
    if args.effect_geometry and not args.effect_layers:
        p.error('--effect-geometry requires --effect-layers')
    if args.archive_provenance:
        from capture_inventory_reference import verify_private_copy
        def verify(provenance, steam, archived):
            audit = verify_private_copy(provenance, steam, archived)
            return dict(audit, build="22529139", method="Archived copy checked against provenance hashes; recorded research build")
    print(capture(args))
