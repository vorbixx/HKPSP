"""Validate saved plant dependencies and record their supported animation subset.

No live AssetRipper request, source mutation or runtime asset installation.
"""
import hashlib
import json
import math
from pathlib import Path

from assetripper_client import ROOT
from extract_plants import BEHAVIOR


def require(ok, message):
    if not ok:
        raise ValueError(message)


def timeline(clip):
    """Validate a root SpriteRenderer-only, event-free looping clip."""
    for name in ('m_FloatCurves', 'm_PositionCurves', 'm_RotationCurves',
                 'm_ScaleCurves', 'm_EulerCurves', 'm_CompressedRotationCurves', 'm_Events'):
        require(not clip[name], 'Unsupported animation channel: ' + name)
    require(not clip['m_Legacy'] and not clip['m_Compressed'], 'Unsupported clip encoding')
    require(len(clip['m_PPtrCurves']) == 1, 'Expected one sprite curve')
    curve = clip['m_PPtrCurves'][0]
    require(curve['m_Path'] == '' and curve['m_ClassID'] == 212 and
            curve['m_Attribute'] == 'm_Sprite' and not curve['m_Script']['m_PathID'],
            'Expected root SpriteRenderer sprite binding')
    info = clip['m_MuscleClipInfo']
    require(info['m_StartTime'] == 0 and info['m_LoopTime'] and
            not info['m_LoopBlend'] and info['m_CycleOffset'] == 0 and
            not info['m_Mirror'], 'Unsupported clip timing')
    duration = info['m_StopTime']
    require(math.isfinite(duration) and duration > 0, 'Invalid clip duration')
    require(math.isfinite(clip['m_SampleRate']) and clip['m_SampleRate'] > 0,
            'Invalid sample rate')
    frames = curve['m_Curve']
    require(frames and frames[0]['m_Time'] == 0, 'Missing initial sprite')
    previous = -1
    for frame in frames:
        time = frame['m_Time']
        require(math.isfinite(time) and previous < time < duration, 'Invalid sprite key time')
        require(frame['m_Value']['m_PathID'] > 0, 'Null sprite key')
        previous = time
    return dict(duration=duration, sample_rate=clip['m_SampleRate'], keys=frames)


def single_state(controller, state_name):
    require(not controller['m_AnimatorParameters'] and not controller['m_StateMachineBehaviours'],
            'Controller parameters/behaviours unresolved')
    require(len(controller['m_AnimationClips']) == 1 and len(controller['m_AnimatorLayers']) == 1,
            'Expected single layer and clip')
    layer = controller['m_AnimatorLayers'][0]
    require(not layer['m_IKPass'] and layer['m_SyncedLayerIndex'] == -1 and
            not layer['m_Behaviours'] and not layer['m_Mask']['m_PathID'] and
            layer['m_BlendingMode'] == 0, 'Unsupported layer')
    machines = controller['m_Controller']['m_StateMachineArray']
    require(len(machines) == 1, 'Expected one state machine')
    machine = machines[0]['m_Data']
    require(machine['m_DefaultState'] == 0 and not machine['m_AnyStateTransitionConstantArray'] and
            len(machine['m_StateConstantArray']) == 1, 'Controller transitions unresolved')
    state = machine['m_StateConstantArray'][0]['m_Data']
    names = {r['Key']: r['Value'] for r in controller['m_TOS']}
    require(names[state['m_NameID']] == state_name, 'Random-start state mismatch')
    require(not state['m_TransitionConstantArray'] and state['m_Speed'] == 1 and
            state['m_CycleOffset'] == 0 and state['m_Loop'] and not state['m_Mirror'] and
            not state['m_IKOnFeet'], 'Unsupported state playback')
    for key in ('m_SpeedParamID', 'm_CycleOffsetParamID', 'm_TimeParamID', 'm_MirrorParamID'):
        require(state[key] == 0, 'Parameterized state playback')
    trees = state['m_BlendTreeConstantArray']
    require(len(trees) == 1 and state['m_BlendTreeConstantIndexArray'] == [0], 'Blend tree unresolved')
    nodes = trees[0]['m_Data']['m_NodeArray']
    require(len(nodes) == 1, 'Expected one clip leaf')
    node = nodes[0]['m_Data']
    require(not node['m_ChildIndices'] and node['m_ClipID'] == 0 and
            node['m_CycleOffset'] == 0 and not node['m_Mirror'], 'Unsupported clip leaf')
    return dict(name=state_name, speed=state['m_Speed'], loop=state['m_Loop'])


def reference(source_key, ref, target_key):
    """Check identity; external collection resolution remains an export observation."""
    collection, path_id = target_key.removesuffix('.json').rsplit('-', 1)
    require(int(path_id) == ref['m_PathID'], 'Dependency path ID mismatch')
    if ref['m_FileID'] == 0:
        require(collection == source_key.rsplit('-', 1)[0], 'Local dependency collection mismatch')


def observe(source, room_source, behavior_source=None):
    report = json.loads((source / 'manifest.json').read_bytes())
    require((report['build'], report['scene']) == (22529139, 'Tutorial_01'), 'Unexpected build/scene')
    if behavior_source is None:
        behavior_source = ROOT / '_private/research/decompiled/all/PlayFromRandomFrameMecanim.cs'
    require(report['behavior_sha256'] == BEHAVIOR and
            hashlib.sha256(behavior_source.read_bytes()).hexdigest() == BEHAVIOR,
            'Changed random-start behavior; recheck observations')
    cache = {}
    for key, expected in report['source_sha256'].items():
        require(key == Path(key).name and '\\' not in key, 'Nonlocal dependency path')
        raw = (source / key).read_bytes()
        require(hashlib.sha256(raw).hexdigest() == expected, 'Changed plant dependency: ' + key)
        if key.endswith('.json'):
            cache[key] = json.loads(raw)
    room_raw = (room_source / 'room-visuals-map.json').read_bytes()
    require(hashlib.sha256(room_raw).hexdigest() == report['room_manifest_sha256'], 'Changed room manifest')
    room = json.loads(room_raw)
    roots = {int(row['go'][7:-5]): row for row in room['inventory']
             if row['active'] and 'Breakable' in row['components'] and
             'PlayFromRandomFrameMecanim' in row['components']}
    owners = [plant['owner'] for plant in report['plants']]
    require(len(owners) == len(set(owners)) and set(owners) == set(roots),
            'Plant inventory mismatch')
    for key, expected in report['room_anchors'].items():
        require(expected == room['source_sha256'][key] and
                hashlib.sha256((room_source / key).read_bytes()).hexdigest() == expected and
                report['source_sha256'][key] == expected, 'Changed room anchor: ' + key)
    clips, plants, sprites = {}, [], set()
    for plant in report['plants']:
        root = roots[plant['owner']]
        component_keys = []
        for entry in cache[root['go']]['m_Components']:
            ref = entry['m_Component']
            require(ref['m_FileID'] == 0, 'External plant component unresolved')
            component_keys.append(f"{root['go'].rsplit('-', 1)[0]}-{ref['m_PathID']}.json")
        require(plant['animator'] in component_keys and
                plant['random_start'] in root['components']['PlayFromRandomFrameMecanim'] and
                plant['renderer'] == root['renderer'], 'Plant component mismatch')
        require(all(key in report['room_anchors'] for key in [root['go']] + component_keys),
                'Missing plant root anchors')
        animator = cache[plant['animator']]
        require('m_Controller' in animator, 'Plant Animator component mismatch')
        require(animator['m_Enabled'] and not animator['m_ApplyRootMotion'] and
                animator['m_UpdateMode'] == 0, 'Unsupported Animator settings')
        script = cache[plant['random_start']]['m_Structure']
        controller = cache[plant['controller']]
        reference(plant['animator'], animator['m_Controller'], plant['controller'])
        state = single_state(controller, script['stateToPlay'])
        require(not script['onEnable'], 'Random-start OnEnable branch unresolved')
        require(len(plant['clips']) == 1, 'Unexpected clip count')
        clip_key = plant['clips'][0]
        reference(plant['controller'], controller['m_AnimationClips'][0], clip_key)
        clip = timeline(cache[clip_key])
        collection = clip_key.rsplit('-', 1)[0]
        for key in clip['keys']:
            ref = key['m_Value']
            require(ref['m_FileID'] == 0, 'External sprite requires saved resolution map')
            sprite_key = f"{collection}-{ref['m_PathID']}.json"
            require(sprite_key in cache and 'm_RD' in cache[sprite_key], 'Sprite dependency missing')
            texture_ref = cache[sprite_key]['m_RD']['m_Texture']
            require(texture_ref['m_FileID'] == 0, 'External texture requires saved resolution map')
            texture_key = f"{collection}-{texture_ref['m_PathID']}.json"
            image_key = report['textures'].get(texture_key)
            require(texture_key in cache and image_key in report['source_sha256'],
                    'Texture dependency missing')
            sprites.add(sprite_key)
        clips[clip_key] = clip
        names = []
        for owner in reversed(root['owners']):
            raw = (room_source / owner).read_bytes()
            require(hashlib.sha256(raw).hexdigest() == room['source_sha256'][owner],
                    'Changed plant hierarchy: ' + owner)
            names.append(json.loads(raw)['m_Name'])
        plants.append(dict(owner=plant['owner'], path=report['scene'] + '/' + '/'.join(names),
                           authored_sprite=root['sprite'], state=state, clip=clip_key,
                           animator_culling_mode=animator['m_CullingMode'],
                           keep_state_on_disable=animator['m_KeepAnimatorStateOnDisable'],
                           write_defaults_on_disable=animator['m_WriteDefaultValuesOnDisable']))
    missing = sorted(sprites - set(room['sprites']))
    return dict(build=report['build'], scene=report['scene'], plants=plants, clips=clips,
                unique_sprites=len(sprites), missing_room_sprites=missing,
                source_manifest_sha256=hashlib.sha256((source / 'manifest.json').read_bytes()).hexdigest(),
                behavior_sha256=report['behavior_sha256'],
                provenance_limitation=report['limitation'],
                observations=['Start requests random normalized phase after one yielded frame.',
                              'Only the root sprite changes; no transform curves or clip events.'],
                unresolved=['Player timing/culling is validated separately by analyze_plant_trace.py.',
                            'GameManager death/retry is validated separately by analyze_plant_retry.py; global random order unresolved.',
                            'Native runtime artwork admission and matched visual acceptance; staging validated separately.'])


if __name__ == '__main__':
    source = ROOT / '_private/research/plant-dependencies'
    report = observe(source, ROOT / '_private/research/room-visuals')
    (source / 'profile.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(dict(plants=len(report['plants']), clips=len(report['clips']),
                         unique_sprites=report['unique_sprites'],
                         missing_room_sprites=len(report['missing_room_sprites']))))
