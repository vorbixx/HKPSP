"""Generate validated Quirrel presentation metadata from level229-source.json."""
import argparse
import json
import math
from pathlib import Path

import converted_files


def one(xs, label):
    if len(xs) != 1:
        raise ValueError(f"Expected one {label}")
    return xs[0]


def actor(src, name):
    return one([x for x in src['actors'] if x.get('name') == name], f'{name} actor')


def comp(a, kind):
    return one([x for x in a['components'] if x['type'] == kind], f'{a["name"]} {kind}')


def fsm(a, name):
    return one([x['decoded'] for x in a['components'] if x['type'] == 'PlayMakerFSM' and
                x.get('decoded', {}).get('name') == name], f'{name} FSM')


def state(f, name):
    return one([x for x in f['states'] if x['name'] == name], f'{name} state')


def acts(f, name, expected):
    source = state(f, name)
    # Wait and chase start together in these original non-sequence states.
    if source.get('isSequence') != 0:
        raise ValueError(f'Unsupported {name} action sequencing')
    xs = [x for x in source['actions'] if x['enabled']]
    got = [x['type'].split(',')[0].split('.')[-1] for x in xs]
    if got != expected:
        raise ValueError(f'Unsupported {name} action order: {got}')
    return xs


def edge(f, s, event, target):
    x = one([x for x in state(f, s)['transitions'] if x['fsmEvent']['name'] == event],
            f'{s}/{event}')
    if x['toState'] != target:
        raise ValueError(f'Unsupported {s}/{event} transition')


def target(p, name):
    go = p['gameObject']
    if name == '':
        if go.get('ownerOption') != 0 or go['gameObject'].get('useVariable'):
            raise ValueError('Expected owner-self animation target')
    elif go.get('ownerOption') != 1 or not go['gameObject'].get('useVariable') or go['gameObject'].get('name') != name:
        raise ValueError(f'Unexpected target; expected {name}')


def anim(a, typ, name, clip, finished=None):
    p = a['parameters']
    if a['type'].split(',')[0].split('.')[-1] != typ or p['clipName'].get('value') != clip:
        raise ValueError(f'Unexpected {clip} animation action')
    target(p, name)
    if p.get('animationCompleteEvent') != finished:
        raise ValueError(f'Unexpected {clip} completion event')


def audio(a, spawn, path):
    p = a['parameters']
    clip = p.get('audioClip', {}).get('value', {})
    if p['spawnPoint'].get('name') != spawn or (clip.get('m_FileID'), clip.get('m_PathID')) != (3, path):
        raise ValueError(f'Unexpected audio action at {spawn}')


def number(v):
    if isinstance(v, dict):
        if v.get('useVariable'):
            raise ValueError('Expected serialized constant')
        v = v['value']
    if not isinstance(v, (float, int)) or not math.isfinite(v):
        raise ValueError('Expected finite numeric value')
    return float(v)


def convert(src):
    if src.get('build_id') != 22529139 or src.get('scene') != 'level229':
        raise ValueError('Expected build22529139 scene level229 metadata')
    land, slash, jelly = (actor(src, n) for n in ('Quirrel Land', 'Quirrel Slash', 'Mega Jellyfish'))
    lt, st = (comp(x, 'Transform')['data'] for x in (land, slash))
    root = comp(jelly, 'Transform')
    rt = root['data']
    if root['path_id'] != 47 or rt['m_LocalScale'] != {'x': 1.0, 'y': 1.0, 'z': 1.0} or rt['m_LocalRotation'] != {'x': 0.0, 'y': 0.0, 'z': 0.0, 'w': 1.0}:
        raise ValueError('Unexpected Mega Jellyfish root transform')
    if not land['active'] or not slash['active'] or any(x['m_Father']['m_PathID'] != 47 for x in (lt, st)):
        raise ValueError('Quirrel actors must be active children of Mega Jellyfish transform 47')
    for child in (lt, st):
        if (child['m_LocalRotation'] != {'x': 0.0, 'y': 0.0, 'z': 0.0, 'w': 1.0} or
                child['m_LocalScale']['y'] != 1 or child['m_LocalScale']['z'] != 1):
            raise ValueError('Unsupported initial Quirrel child rotation/scale')
    lv = bool(comp(land, 'MeshRenderer')['data']['m_Enabled'])
    sv = bool(comp(slash, 'MeshRenderer')['data']['m_Enabled'])
    if lv or sv:
        raise ValueError('Quirrel actors must start hidden')
    f = fsm(jelly, 'Mega Jellyfish')
    check = acts(f, 'Check Pos', ['GetPosition', 'AudioPlayerOneShotSingle', 'FloatTestToBool', 'FloatTestToBool', 'BoolTestMulti', 'BoolTestMulti', 'BoolTestMulti', 'SendEvent'])
    audio(check[1], 'Quirrel Land', 11)
    arrive = acts(f, 'Q Arrive', ['SetMeshRenderer', 'AudioPlayerOneShotSingle', 'Tk2dPlayAnimation', 'Wait', 'ChaseObjectV2'])
    target(arrive[0]['parameters'], 'Quirrel Land')
    if arrive[0]['parameters']['active'].get('value') is not True or number(arrive[3]['parameters']['time']) != 1.5:
        raise ValueError('Unexpected Q Arrive renderer or wait')
    audio(arrive[1], 'Quirrel Slash', 7)
    anim(arrive[2], 'Tk2dPlayAnimation', 'Quirrel Land', 'Quirrel Arrive')
    edge(f, 'Q Arrive', 'ATTACK', 'Q Leave')
    leave = acts(f, 'Q Leave', ['ChaseObjectV2', 'AudioPlayerOneShotSingle', 'Tk2dPlayAnimation', 'Tk2dPlayAnimationWithEvents'])
    audio(leave[1], 'Quirrel Slash', 18)
    anim(leave[2], 'Tk2dPlayAnimation', 'Quirrel Land', 'Quirrel Leave')
    anim(leave[3], 'Tk2dPlayAnimationWithEvents', 'Quirrel Land', 'Quirrel Leave', 'FINISHED')
    edge(f, 'Q Leave', 'FINISHED', 'Slash Pause')
    antic = acts(f, 'Slash Antic', ['DecelerateV2', 'AudioPlayerOneShot', 'AudioPlayerOneShotSingle', 'RandomFloat', 'RandomFloatEither', 'SetRotation', 'SetScale', 'SetMeshRenderer', 'Tk2dPlayAnimationWithEvents'])
    if not math.isclose(number(antic[0]['parameters']['deceleration']), .85, abs_tol=1e-6):
        raise ValueError('Unexpected Slash Antic damping')
    weighted = antic[1]['parameters']
    if [(x['m_FileID'], x['m_PathID']) for x in weighted['audioClips']['elements']] != [(3, 16), (3, 15)] or [x['value'] for x in weighted['weights']['elements']] != [1, 1] or weighted['spawnPoint']['name'] != 'Quirrel Slash':
        raise ValueError('Unexpected Slash Antic weighted audio')
    audio(antic[2], 'Quirrel Slash', 7)
    if antic[3]['parameters']['storeResult']['variable'] != 'Slash Rotation' or antic[4]['parameters']['storeResult']['variable'] != 'Slash Scale':
        raise ValueError('Unexpected Slash Antic RNG bindings')
    target(antic[5]['parameters'], 'Quirrel Slash'); target(antic[6]['parameters'], 'Quirrel Slash')
    if antic[5]['parameters']['zAngle'].get('variable') != 'Slash Rotation' or antic[6]['parameters']['x'].get('variable') != 'Slash Scale':
        raise ValueError('Unexpected Slash Antic pose bindings')
    target(antic[7]['parameters'], 'Quirrel Slash')
    if antic[7]['parameters']['active'].get('value') is not True:
        raise ValueError('Slash Antic must show Quirrel Slash')
    anim(antic[8], 'Tk2dPlayAnimationWithEvents', 'Quirrel Slash', 'Slash Antic', 'FINISHED')
    edge(f, 'Slash Antic', 'FINISHED', 'Slash')
    slash_actions = acts(f, 'Slash', ['Tk2dPlayAnimationWithEvents', 'AudioPlayerOneShotSingle', 'AudioPlaySimple', 'SendEventByName', 'Tk2dPlayAnimation', 'DecelerateV2', 'SetInvincible', 'DestroyComponent', 'SetCircleCollider', 'PlayParticleEmitter', 'ActivateGameObject', 'ActivateGameObject'])
    anim(slash_actions[0], 'Tk2dPlayAnimationWithEvents', 'Quirrel Slash', 'Slash', 'FINISHED')
    audio(slash_actions[1], 'Quirrel Slash', 8)
    wound_audio = slash_actions[2]['parameters']
    wound_clip = wound_audio['oneShotClip']['value']
    wound_owner = wound_audio['gameObject']
    if (wound_owner.get('ownerOption') != 0 or wound_owner['gameObject'].get('useVariable') or
            (wound_clip.get('m_FileID'), wound_clip.get('m_PathID')) != (3, 17)):
        raise ValueError('Unexpected parent-position Wound audio action')
    anim(slash_actions[4], 'Tk2dPlayAnimation', '', 'Wound')
    edge(f, 'Slash', 'FINISHED', 'Wounded')
    wounded = acts(f, 'Wounded', ['SetAudioPitch', 'SetMeshRenderer', 'ChaseObjectV2', 'Wait', 'SetRecoilSpeed'])
    target(wounded[1]['parameters'], 'Quirrel Slash')
    if wounded[1]['parameters']['active'].get('value') is not False or number(wounded[3]['parameters']['time']) != 3:
        raise ValueError('Unexpected Wounded hide/wait')
    edge(f, 'Wounded', 'FINISHED', 'Recover Antic')
    return {
        'landArriveClip': 'Quirrel Arrive', 'landLeaveClip': 'Quirrel Leave',
        'slashAnticClip': 'Slash Antic', 'slashClip': 'Slash',
        # The shared HKA1 converter namespaces original clips by actor.
        'landArriveBankClip': 'Quirrel/Quirrel Arrive',
        'landLeaveBankClip': 'Quirrel/Quirrel Leave',
        'landWatchBankClip': 'Quirrel/Quirrel Watch',
        'slashAnticBankClip': 'Quirrel/Slash Antic', 'slashBankClip': 'Quirrel/Slash',
        **{f'{obj}Local{axis.upper()}': number(data['m_LocalPosition'][axis])
           for obj, data in (('land', lt), ('slash', st)) for axis in 'xyz'},
        **{f'{obj}Scale{axis.upper()}': number(data['m_LocalScale'][axis])
           for obj, data in (('land', lt), ('slash', st)) for axis in 'xyz'},
        'landVisible': lv, 'slashVisible': sv,
        'slashAudioFile': slash_actions[1]['parameters']['audioClip']['value']['m_FileID'],
        'slashAudioPath': slash_actions[1]['parameters']['audioClip']['value']['m_PathID'],
        'woundAudioFile': wound_clip['m_FileID'], 'woundAudioPath': wound_clip['m_PathID'],
    }


def header(facts):
    lines = ['// Generated from build 22529139, scene level229 by tools/convert_uumuu_quirrel.py.', '#pragma once', '#include <cstdint>', '#include <string_view>', 'namespace hk3ds::uumuu_quirrel_data {']
    for k, v in facts.items():
        if isinstance(v, str): lines.append(f'inline constexpr std::string_view {k}="{v}";')
        elif isinstance(v, bool): lines.append(f'inline constexpr bool {k}={str(v).lower()};')
        elif isinstance(v, int): lines.append(f'inline constexpr std::uint32_t {k}={v};')
        else: lines.append(f'inline constexpr float {k}={v:.9f}f;')
    return '\n'.join(lines + ['}']) + '\n'


def main():
    root = Path(__file__).resolve().parents[1]
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, default=root/'_private/minimum-bosses/uumuu/level229-source.json')
    p.add_argument('--output', type=Path, default=root/'include/uumuu_quirrel_data.hpp')
    p.add_argument('--check', action='store_true')
    a = p.parse_args()
    facts = convert(json.loads(a.source.read_text(encoding='utf-8')))
    text = header(facts)
    if a.check:
        if not a.output.exists() or a.output.read_text(encoding='utf-8') != text:
            raise SystemExit('generated Uumuu Quirrel metadata is stale')
    else:
        a.output.write_text(text, encoding='utf-8')
    print(json.dumps(facts, sort_keys=True))


if __name__ == '__main__': main()
