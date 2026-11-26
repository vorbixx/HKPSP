"""Generate validated Quirrel Land Watch FSM data from level229-source.json."""
import converted_files  # Protect this worktree's converted outputs.
import argparse
import json
import math
from pathlib import Path

import convert_uumuu_quirrel as common


CHOICES = ('BBL', 'BBR', 'BL', 'BR', 'TL', 'TR', 'TTL', 'TTR')
CYCLE = ('BL', 'BR', 'TL', 'TR', 'BBL', 'BBR', 'TTL', 'TTR')
EXPECTED_STATES = {
    'Idle', 'Pause', 'Check Pos', *CYCLE, 'Battle Ended', 'Voice',
    'Voice Intro', 'Anim', 'Leap Out SFX', 'Renderer Off',
}
EXPECTED_VOICE_CLIPS = ((3, 14), (3, 10), (3, 13), (0, 0), (0, 0))


def require(condition, message):
    if not condition:
        raise ValueError(message)


def edges(fsm, state_name, expected):
    got = [(x['fsmEvent']['name'], x['toState'])
           for x in common.state(fsm, state_name)['transitions']]
    require(got == expected, f'Unexpected {state_name} transitions: {got}')


def variable(fsm, group, name):
    return common.one([x for x in fsm['variables'][group] if x.get('name') == name],
                      f'{name} FSM variable')


def constant(value, label):
    require(isinstance(value, dict) and value.get('useVariable') is False,
            f'{label} must be a serialized constant')
    return common.number(value)


def variable_value(value, label):
    require(isinstance(value, dict) and value.get('useVariable') is True,
            f'{label} must retain its serialized variable binding')
    require(value.get('value') == 0, f'{label} serialized value changed')
    return 0.0


def ref_id(value, label):
    require(isinstance(value, dict), f'Missing {label} reference')
    return int(value.get('m_FileID', -1)), int(value.get('m_PathID', -1))


def clip_ref(p, label):
    return ref_id(p['value'], label)


def check_spawn(p, label):
    spawn = p['spawnPoint']
    require(spawn.get('useVariable') == 1 and spawn.get('name') == 'Self',
            f'{label} must spawn at Self')


def check_player(p, label):
    require(ref_id(p['audioPlayer']['value'], f'{label} audio player') == (4, 4126),
            f'Unexpected {label} audio player')


def check_single_audio(action, path, pitch=(1.0, 1.0), label='audio'):
    p = action['parameters']
    check_player(p, label)
    check_spawn(p, label)
    require(clip_ref(p['audioClip'], label) == (3, path), f'Unexpected {label} clip')
    require(math.isclose(common.number(p['pitchMin']), pitch[0], abs_tol=1e-6) and
            math.isclose(common.number(p['pitchMax']), pitch[1], abs_tol=1e-6),
            f'Unexpected {label} pitch range')
    require(common.number(p['volume']) == 1 and common.number(p['delay']) == 0,
            f'Unexpected {label} volume or delay')


def check_renderer(action, active):
    p = action['parameters']
    common.target(p, '')
    require(p['active'].get('value') is active and p['active'].get('useVariable') is False,
            'Unexpected Watch renderer visibility')


def check_land_transform_target(p):
    go = p['gameObject']
    target = go['gameObject']
    require(go.get('ownerOption') == 0 and target.get('useVariable') == 1 and
            target.get('name') == 'Quirrel Land',
            'Watch transform action must target owner-self Quirrel Land')


def _voice_data(fsm):
    voice, intro_test = common.acts(fsm, 'Voice', ['AudioPlayerOneShot', 'BoolTest'])
    p = voice['parameters']
    check_player(p, 'Voice')
    check_spawn(p, 'Voice')
    refs = tuple(ref_id(x, 'Voice clip') for x in p['audioClips']['elements'])
    require(refs == EXPECTED_VOICE_CLIPS, 'Unexpected weighted Voice clips')
    weights = p['weights']['elements']
    require(len(weights) == len(refs) and all(constant(x, 'Voice weight') == 1 for x in weights),
            'Voice weights must all be one')
    require(common.number(p['pitchMin']) == 1 and common.number(p['pitchMax']) == 1 and
            common.number(p['volume']) == 1 and common.number(p['delay']) == 0,
            'Unexpected Voice audio settings')
    test = intro_test['parameters']
    require(test['boolVariable'].get('variable') == 'Voice Intro' and
            test.get('isTrue') == '' and test.get('isFalse') == 'INTRO',
            'Unexpected Voice intro branch')
    intro_actions = common.acts(fsm, 'Voice Intro', ['SetBoolValue', 'AudioPlayerOneShotSingle'])
    flag = intro_actions[0]['parameters']
    require(flag['boolVariable'].get('variable') == 'Voice Intro' and
            flag['boolValue'].get('value') is True and flag['boolValue'].get('useVariable') is False,
            'Unexpected Voice Intro flag action')
    check_single_audio(intro_actions[1], 11, label='Voice intro')
    edges(fsm, 'Voice Intro', [('FINISHED', 'Anim')])
    edges(fsm, 'Voice', [('INTRO', 'Voice Intro'), ('FINISHED', 'Anim')])
    return refs, tuple(common.number(x) for x in weights)


def convert(src):
    if src.get('build_id') != 22529139 or src.get('scene') != 'level229':
        raise ValueError('Expected build22529139 scene level229 metadata')
    land = common.actor(src, 'Quirrel Land')
    fsm = common.fsm(land, 'Watch')
    require(fsm.get('start') == 'Idle', 'Watch FSM must start in Idle')
    require(fsm.get('globalTransitions') == [], 'Watch FSM must have no global transitions')
    require({x['name'] for x in fsm['states']} == EXPECTED_STATES,
            'Unexpected Watch FSM state set')

    roam = variable(fsm, 'boolVariables', 'Roam')
    intro = variable(fsm, 'boolVariables', 'Voice Intro')
    previous = variable(fsm, 'stringVariables', 'Prev Pos')
    require(roam.get('value') == 0 and intro.get('value') == 0 and previous.get('value') == '',
            'Unexpected Watch FSM variable defaults')
    require(all(x.get('isGlobal') == 0 for x in fsm['events'] if x.get('name') == 'WATCH'),
            'WATCH must be a local event')

    idle = common.acts(fsm, 'Idle', ['GetOwner', 'GameObjectIsNull', 'BoolTest'])
    require(idle[0]['parameters']['storeGameObject'].get('name') == 'Self',
            'Unexpected Watch owner binding')
    is_null = idle[1]['parameters']
    require(is_null['gameObject'].get('name') == 'Jellyfish' and
            is_null.get('isNull') == 'END' and is_null.get('isNotNull') == '',
            'Unexpected Watch Jellyfish guard')
    roam_test = idle[2]['parameters']
    require(roam_test['boolVariable'].get('variable') == 'Roam' and
            roam_test.get('isTrue') == 'WATCH' and roam_test.get('isFalse') == '',
            'Unexpected Watch Roam branch')
    require(is_null.get('everyFrame') is False and roam_test.get('everyFrame') is False,
            'Watch Idle guards must run only on state entry')
    edges(fsm, 'Idle', [('WATCH', 'Pause'), ('END', 'Battle Ended')])
    require(common.state(fsm, 'Battle Ended')['actions'] == [] and
            common.state(fsm, 'Battle Ended')['transitions'] == [],
            'Unexpected Battle Ended state')

    pause, = common.acts(fsm, 'Pause', ['WaitRandom'])
    wait = pause['parameters']
    pause_min = common.number(wait['timeMin'])
    pause_max = common.number(wait['timeMax'])
    require((pause_min, pause_max, wait.get('finishEvent'), wait.get('realTime')) ==
            (0.5, 0.75, 'FINISHED', False), 'Unexpected Watch pause')
    edges(fsm, 'Pause', [('FINISHED', 'Check Pos')])

    random_action, = common.acts(fsm, 'Check Pos', ['SendRandomEvent'])
    random = random_action['parameters']
    require(tuple(random['events']['elements']) == CHOICES,
            'Unexpected Watch choice event order')
    choice_weights = random['weights']['elements']
    require(len(choice_weights) == len(CHOICES) and
            all(constant(x, 'Watch choice weight') == 1 for x in choice_weights),
            'Watch choices must have equal unit weights')
    require(common.number(random['delay']) == 0,
            'Watch choices must dispatch without delay')
    edges(fsm, 'Check Pos', [(name, name) for name in CYCLE])

    positions = []
    for index, name in enumerate(CYCLE):
        next_name = CYCLE[(index + 1) % len(CYCLE)]
        actions = common.acts(fsm, name, ['StringCompare', 'SetStringValue', 'SetPosition', 'SetScale'])
        compare = actions[0]['parameters']
        require(compare['stringVariable'].get('name') == 'Prev Pos' and
                compare['compareTo'].get('value') == name and compare.get('equalEvent') == 'NEXT' and
                compare.get('notEqualEvent') == '', f'Unexpected {name} repeat comparison')
        set_name = actions[1]['parameters']
        require(set_name['stringVariable'].get('name') == 'Prev Pos' and
                set_name['stringValue'].get('value') == name, f'Unexpected {name} Prev Pos write')
        set_pos = actions[2]['parameters']
        check_land_transform_target(set_pos)
        x, y = common.number(set_pos['x']), common.number(set_pos['y'])
        z = variable_value(set_pos['z'], f'{name} position z')
        require(set_pos.get('space') == 0 and set_pos.get('lateUpdate') is False,
                f'Unexpected {name} position space/update')
        set_scale = actions[3]['parameters']
        check_land_transform_target(set_scale)
        scale_x = common.number(set_scale['x'])
        scale_y = variable_value(set_scale['y'], f'{name} scale y')
        scale_z = variable_value(set_scale['z'], f'{name} scale z')
        require(scale_x in (-1.0, 1.0), f'Unexpected {name} horizontal facing')
        positions.append({
            'name': name, 'next': next_name, 'x': x, 'y': y, 'scaleX': scale_x,
            'positionZ': z, 'positionZUseVariable': True,
            'scaleY': scale_y, 'scaleYUseVariable': True,
            'scaleZ': scale_z, 'scaleZUseVariable': True,
        })
        edges(fsm, name, [('FINISHED', 'Voice'), ('NEXT', next_name)])

    voice_clips, voice_weights = _voice_data(fsm)
    anim = common.acts(fsm, 'Anim', [
        'Tk2dPlayAnimationWithEvents', 'SetMeshRenderer', 'AudioPlayerOneShotSingle', 'Wait',
    ])
    animation = anim[0]['parameters']
    common.target(animation, '')
    require(animation['clipName'].get('value') == 'Quirrel Watch' and
            animation.get('animationTriggerEvent') == '' and
            animation.get('animationCompleteEvent') == '', 'Unexpected Watch animation action')
    check_renderer(anim[1], True)
    check_single_audio(anim[2], 7, (0.9, 1.1), 'Watch start')
    animation_wait = anim[3]['parameters']
    require(math.isclose(common.number(animation_wait['time']), 1.1, abs_tol=1e-6) and
            animation_wait.get('finishEvent') == 'NEXT' and
            animation_wait.get('realTime') is False, 'Unexpected Watch animation wait')
    edges(fsm, 'Anim', [('NEXT', 'Leap Out SFX')])

    leap = common.acts(fsm, 'Leap Out SFX', ['AudioPlayerOneShotSingle', 'Tk2dWatchAnimationEvents'])
    check_single_audio(leap[0], 18, (0.9, 1.1), 'Watch leap')
    watcher = leap[1]['parameters']
    common.target(watcher, '')
    require(watcher.get('animationTriggerEvent') == '' and
            watcher.get('animationCompleteEvent') == 'NEXT',
            'Unexpected Watch animation completion watcher')
    edges(fsm, 'Leap Out SFX', [('NEXT', 'Renderer Off')])

    renderer_off, = common.acts(fsm, 'Renderer Off', ['SetMeshRenderer'])
    check_renderer(renderer_off, False)
    edges(fsm, 'Renderer Off', [('FINISHED', 'Idle')])

    return {
        'start': 'Idle', 'watchEvent': 'WATCH', 'initialRoam': False,
        'initialVoiceIntro': False, 'initialPrevPos': '',
        'choiceNames': CHOICES, 'positions': positions,
        'pauseMin': pause_min, 'pauseMax': pause_max,
        'positionSpace': 0,
        'watchPositionZ': 0.0, 'watchPositionZUseVariable': True,
        'watchScaleY': 0.0, 'watchScaleYUseVariable': True,
        'watchScaleZ': 0.0, 'watchScaleZUseVariable': True,
        'watchClip': 'Quirrel Watch', 'watchBankClip': 'Quirrel/Quirrel Watch',
        'animationWait': 1.1,
        'voiceClips': voice_clips, 'voiceWeights': voice_weights,
        'voiceIntroClip': (3, 11), 'watchStartClip': (3, 7), 'watchLeapClip': (3, 18),
        'audioPlayer': (4, 4126), 'watchPitchMin': 0.9, 'watchPitchMax': 1.1,
    }


def header(facts):
    def flt(value):
        return f'{value:.9f}f'

    rows = []
    for p in facts['positions']:
        rows.append('    WatchPosition{' + ', '.join((
            flt(p['x']), flt(p['y']), flt(p['scaleX']),
            f'"{p["name"]}"', f'"{p["next"]}"')) + '},')
    clips = ', '.join(f'AudioReference{{{file}u, {path}u}}' for file, path in facts['voiceClips'])
    weights = ', '.join(flt(x) for x in facts['voiceWeights'])
    return '\n'.join([
        '// Generated from build 22529139, scene level229 by tools/convert_uumuu_quirrel_watch.py.',
        '#pragma once', '#include <array>', '#include <cstdint>', '#include <string_view>',
        'namespace hk3ds::uumuu_quirrel_watch_data {',
        'struct WatchPosition { float x, y, scaleX; std::string_view name, next; };',
        'struct AudioReference { std::uint32_t file, path; };',
        'inline constexpr std::string_view start="Idle";',
        'inline constexpr std::string_view watchEvent="WATCH";',
        f'inline constexpr bool initialRoam={str(facts["initialRoam"]).lower()};',
        f'inline constexpr bool initialVoiceIntro={str(facts["initialVoiceIntro"]).lower()};',
        'inline constexpr std::string_view initialPrevPos="";',
        f'inline constexpr std::array<std::string_view, {len(facts["choiceNames"])}> watchChoiceNames{{{{' +
        ', '.join(f'"{name}"' for name in facts['choiceNames']) + '}};',
        f'inline constexpr std::array<WatchPosition, {len(rows)}> watchPositions{{{{',
        *rows, '}};',
        f'inline constexpr float pauseMin={flt(facts["pauseMin"])};',
        f'inline constexpr float pauseMax={flt(facts["pauseMax"])};',
        f'inline constexpr std::uint32_t positionSpace={facts["positionSpace"]}u;',
        f'inline constexpr float watchPositionZ={flt(facts["watchPositionZ"])};',
        f'inline constexpr bool watchPositionZUseVariable={str(facts["watchPositionZUseVariable"]).lower()};',
        f'inline constexpr float watchScaleY={flt(facts["watchScaleY"])};',
        f'inline constexpr bool watchScaleYUseVariable={str(facts["watchScaleYUseVariable"]).lower()};',
        f'inline constexpr float watchScaleZ={flt(facts["watchScaleZ"])};',
        f'inline constexpr bool watchScaleZUseVariable={str(facts["watchScaleZUseVariable"]).lower()};',
        f'inline constexpr std::string_view watchClip="{facts["watchClip"]}";',
        f'inline constexpr std::string_view watchBankClip="{facts["watchBankClip"]}";',
        f'inline constexpr float animationWait={flt(facts["animationWait"])};',
        f'inline constexpr std::array<AudioReference, {len(facts["voiceClips"])}> voiceClips{{{{{clips}}}}};',
        f'inline constexpr std::array<float, {len(facts["voiceWeights"])}> voiceWeights{{{{{weights}}}}};',
        f'inline constexpr AudioReference voiceIntroClip{{{facts["voiceIntroClip"][0]}u, {facts["voiceIntroClip"][1]}u}};',
        f'inline constexpr AudioReference watchStartClip{{{facts["watchStartClip"][0]}u, {facts["watchStartClip"][1]}u}};',
        f'inline constexpr AudioReference watchLeapClip{{{facts["watchLeapClip"][0]}u, {facts["watchLeapClip"][1]}u}};',
        f'inline constexpr AudioReference audioPlayer{{{facts["audioPlayer"][0]}u, {facts["audioPlayer"][1]}u}};',
        f'inline constexpr float watchPitchMin={flt(facts["watchPitchMin"])};',
        f'inline constexpr float watchPitchMax={flt(facts["watchPitchMax"])};',
        '}', '',
    ])


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=root/'_private/minimum-bosses/uumuu/level229-source.json')
    parser.add_argument('--output', type=Path, default=root/'include/uumuu_quirrel_watch_data.hpp')
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    text = header(convert(json.loads(args.source.read_text(encoding='utf-8'))))
    if args.check:
        if not args.output.exists() or args.output.read_text(encoding='utf-8') != text:
            raise SystemExit('generated Uumuu Quirrel Watch metadata is stale')
    else:
        args.output.write_text(text, encoding='utf-8')
    print('validated Quirrel Watch source metadata')


if __name__ == '__main__':
    main()
