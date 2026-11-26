"""Source controllers for the Fungus2_21 City Crest socket and front gate."""
import argparse
import json
import math
from pathlib import Path
import struct
import re
import xml.etree.ElementTree as ET

import converted_files
from assetripper_client import ROOT
from inspect_fsm import decode_fsm
from private_output_paths import is_private_output
from convert_terrain import transform_point

SOURCE = ROOT / '_private/research/crossroads-scenes/Fungus2_21/terrain'
FORMAT = struct.Struct('<4s19f')
ROOM_FORMAT = struct.Struct('<4s4I17f768s768s32s32s32s')
MAP_ZONES = ROOT / '_private/research/decompiled/all/GlobalEnums/MapZone.cs'
MARKER = ROOT / '_private/minimum-obstacles/city-crest-source'
PROMPTS = ROOT / '_private/research/tutorial-reminders/EN_Prompts-decrypted.xml'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def city_map_zone():
    source = MAP_ZONES.read_text(encoding='utf-8')
    body = re.search(r'public enum MapZone\s*\{([^}]+)\}', source)
    require(body is not None, 'Missing source MapZone enum')
    names = [v.strip() for v in body[1].split(',') if v.strip()]
    require(all(re.fullmatch(r'[A-Z][A-Z_0-9]*', name) for name in names) and 'CITY' in names,
            'Unsupported source MapZone enum')
    return names.index('CITY')


def transform_chain(read, go):
    transform = next(read(c['m_Component']['m_PathID']) for c in read(go)['m_Components']
                     if 'm_LocalPosition' in read(c['m_Component']['m_PathID']))
    result = [transform]
    while transform['m_Father']['m_PathID']:
        require(not transform['m_Father']['m_FileID'] and len(result) < 64, 'Unsupported gate hierarchy')
        transform = read(transform['m_Father']['m_PathID'])
        result.append(transform)
    require(all(t['m_LocalRotation'] == dict(m_X=0,m_Y=0,m_Z=0,m_W=1) for t in result),
            'Unsupported rotated gate trigger')
    return result


def controller_data(read):
    socket = decode_fsm(read(5567)['m_Structure']['fsm'])
    gate = decode_fsm(read(5613)['m_Structure']['fsm'])
    require(socket['name'] == 'Conversation Control' and gate['name'] == 'Open', 'Changed City Crest FSMs')

    def actions(fsm, state, kind):
        return [a['parameters'] for s in fsm['states'] if s['name'] == state
                for a in s['actions'] if a.get('enabled', True) and a['type'].endswith('.' + kind)]

    def one(fsm, state, kind):
        result = actions(fsm, state, kind)
        require(len(result) == 1, f'Changed City Crest {state}/{kind}')
        return result[0]

    def wait(fsm, state):
        p = one(fsm, state, 'Wait')
        require(not p['time']['useVariable'] and not p['realTime'], 'Changed City Crest wait clock')
        return p['time']['value']

    for fsm, state, expected in ((socket, 'Key?', 'hasCityKey'), (socket, 'Init', 'openedCityGate')):
        require(one(fsm, state, 'PlayerDataBoolTest')['boolName']['value'] == expected,
                'Changed City Crest admission')
    writes = actions(socket, 'Activate', 'SetPlayerDataBool')
    require([(p['boolName']['value'], p['value']['value']) for p in writes] ==
            [('hasCityKey', False), ('openedCityGate', True)], 'Changed City Crest consumption')
    require(one(socket, 'Open', 'SendEventByName')['sendEvent']['value'] == 'GATE OPEN', 'Changed gate event')
    require([(p['boolName']['value'], p['isTrue']) for p in actions(gate, 'Check', 'PlayerDataBoolTest')] ==
            [('cityGateClosed', 'FINISHED'), ('openedCityGate', 'OPENED')], 'Changed gate restore precedence')
    require(one(gate, 'Slam', 'SetPlayerDataBool')['boolName']['value'] == 'cityGateClosed' and
            one(gate, 'Slam', 'SetPlayerDataBool')['value']['value'], 'Changed gate slam persistence')
    calls = actions(gate, 'Slam', 'CallMethodProper')
    require([p['methodName']['value'] for p in calls] == ['SetMapZoneToSpecific', 'SetBenchRespawn'],
            'Changed gate slam side effects')
    require(calls[0]['parameters']['elements'][0]['stringValue'] == 'CITY', 'Changed gate map zone')
    respawn = calls[1]['parameters']['elements']
    require([p['stringValue'] for p in respawn[:2]] == ['Death Respawn Marker', 'Fungus2_21'] and
            respawn[2]['intValue'] == 0 and respawn[3]['boolValue'], 'Changed gate death respawn')

    def move(state, ease, count=1):
        values = actions(gate, state, 'iTweenMoveBy')
        require(len(values) == count, 'Changed gate tween count')
        result = []
        for p, expected in zip(values, ease):
            require(p['easeType'] == expected and p['speed']['useVariable'] and
                    p['vector']['value']['x'] == p['vector']['value']['z'] == 0 and
                    not p['time']['useVariable'] and not p['realTime']['value'] and p['stopOnExit']['value'],
                    'Changed gate tween semantics')
            result.append((p['vector']['value']['y'], p['time']['value'], p['delay']['value']))
        return result

    first, = move('Open 1', (13,))
    second, = move('Open 2', (12,))
    bump, recovery = move('Bump', (13, 12), 2)
    drop, = move('Drop', (13,))
    require(first[2] == second[2] == bump[2] == drop[2] == 0 and recovery[0] == -bump[0] and
            recovery[2] == bump[1], 'Changed gate tween sequencing')
    restored = one(gate, 'To Opened', 'Translate')
    require(not restored['y']['useVariable'] and not restored['perSecond'] and not restored['everyFrame'],
            'Changed restored gate translation')
    slam = one(gate, 'Slam', 'iTweenMoveTo')
    initial = read(3186)['m_LocalPosition']
    # MoveBy/Translate displacements are world units for this unrotated gate.
    # MoveTo(Self) interpolates localPosition, including its speed-derived time.
    parent_scale = math.prod(t['m_LocalScale']['m_Y'] for t in transform_chain(read, 1466)[1:])
    require(math.isfinite(parent_scale) and parent_scale > 0, 'Unsupported moving gate scale')
    require(slam['space'] == 1 and slam['easeType'] == 21 and slam['time']['useVariable'] and
            not slam['speed']['useVariable'] and not slam['realTime']['value'] and slam['stopOnExit']['value'] and
            abs(slam['vectorPosition']['value']['x'] - initial['m_X']) < .00001,
            'Changed local gate slam')
    return [wait(socket, s) for s in ('Box Up', 'Box Up YN', 'Decline Pause', 'Box Down', 'Yes', 'Activate')] + [
        first[0], first[1], wait(gate, 'Open 1'), second[0], second[1], bump[0], bump[1], recovery[1],
        drop[0], drop[1], restored['y']['value'],
        (slam['vectorPosition']['value']['y'] - initial['m_Y']) * parent_scale,
        slam['speed']['value'] * parent_scale]


def room_data(read, marker, prompts):
    def chain(go):
        return transform_chain(read, go)

    def trigger(ident):
        c = read(ident)
        require(c['m_Enabled'] and c['m_IsTrigger'], 'Missing gate interaction trigger')
        transforms = chain(c['m_GameObject']['m_PathID'])
        o, size = c['m_Offset'], c['m_Size']
        a = transform_point((o['m_X']-size['m_X']/2, o['m_Y']-size['m_Y']/2), transforms)
        b = transform_point((o['m_X']+size['m_X']/2, o['m_Y']+size['m_Y']/2), transforms)
        return (min(a[0],b[0]), min(a[1],b[1]), max(a[0],b[0]), max(a[1],b[1]))

    npc = decode_fsm(read(5569)['m_Structure']['fsm'])
    variables = {v['name']: v['value'] for kind in ('boolVariables','floatVariables','stringVariables')
                 for v in npc['variables'][kind]}
    require(variables['Hero Always Left'] and not variables['Hero Always Right'] and variables['Prompt Name'] == 'Inspect',
            'Changed gate inspect approach')
    velocity = [a['parameters']['x'] for s in npc['states'] if s['name'] == 'Move Hero Left'
                for a in s['actions'] if a.get('enabled',True) and a['type'].endswith('.SetVelocity2d')]
    require(len(velocity)==1 and not velocity[0]['useVariable'] and velocity[0]['value']<0, 'Changed gate approach speed')
    report = json.loads((marker/'sources.json').read_text())
    require(report['scene']=='Fungus2_21' and report['name']=='Death Respawn Marker' and
            report['spawnType']==0 and report['facingRight'], 'Changed gate respawn source')
    respawn = transform_point((0,0), [json.loads((marker/name).read_text()) for name in report['transforms']])
    require(read(1466)['m_Name']=='Ruins_gate_main' and read(3830)['m_GameObject']['m_PathID']==1466 and
            not read(3830)['m_IsTrigger'] and not read(1617)['m_IsActive'], 'Changed gate collider ownership')
    position = transform_point((0,0), chain(704))
    def text(key, size):
        value = re.sub(r'<[^>]*>', '', (prompts[key] or '').replace('<br>', '\n')).encode('utf-8')
        require(0<len(value)<size, 'Gate dialogue exceeds bounded storage')
        return value
    values = [*trigger(3806), *trigger(3799), *trigger(3804), position[0], variables['Move To Offset'],
              -velocity[0]['value'], *respawn]
    return ROOM_FORMAT.pack(b'CRR2',1466,3830,3837,city_map_zone(),*values,text('CITY_GATE_NOKEY',768),text('CITY_GATE_KEY',768),
                            text('YES',32),text('NO',32),text('INSPECT',32))


def audio_position(read):
    """Admit the authored gate sources before sharing the Town spatial curve."""
    gate = decode_fsm(read(5613)['m_Structure']['fsm'])
    for state, reference in (('Open 1', (3, 107)), ('Bump', (3, 98)),
                             ('Drop', (2, 161)), ('Slammed', (2, 171))):
        actions = [a['parameters'] for s in gate['states'] if s['name'] == state
                   for a in s['actions'] if a.get('enabled', True)
                   and a['type'].endswith('.AudioPlayerOneShotSingle')]
        require(len(actions) == 1, 'Changed City Crest sound state')
        p = actions[0]
        require(p['audioClip']['value'] == dict(m_FileID=reference[0], m_PathID=reference[1])
                and p['audioPlayer']['value'] == dict(m_FileID=4, m_PathID=4126)
                and p['spawnPoint']['name'] == 'Self'
                and all(not p[k]['useVariable'] and p[k]['value'] == v
                        for k, v in (('pitchMin', 1), ('pitchMax', 1), ('volume', 1), ('delay', 0))),
                'Changed City Crest sound parameters')
    source = read(3879)
    require(source['m_GameObject']['m_PathID'] == 1466 and source['m_Loop']
            and not source['m_PlayOnAwake'] and source['m_Pitch'] == source['m_Volume'] == 1
            and source['m_Resource'] == dict(m_FileID=3, m_PathID=100)
            and source['m_RolloffMode'] == 2 and source['m_MaxDistance'] == 50,
            'Changed City Crest movement AudioSource')
    curve = source['m_RolloffCustomCurve']['m_Curve']
    require([(k['m_Time'], k['m_Value'], k['m_InSlope'], k['m_OutSlope']) for k in curve]
            == [(.78, 1, 0, -4.545454), (.9411765, 0, -9.596051, -9.596051)],
            'Changed City Crest audio rolloff')
    position = [0., 0., 0.]
    for transform in transform_chain(read, 1466):
        for i, key in enumerate(('m_X', 'm_Y', 'm_Z')):
            position[i] = position[i] * transform['m_LocalScale'][key] + transform['m_LocalPosition'][key]
    require(all(math.isfinite(v) for v in position), 'Invalid gate sound position')
    return position


def convert(source, output, marker=MARKER, prompts=PROMPTS):
    require(is_private_output(output), 'City Crest output must remain private')
    read = lambda ident: json.loads((source / f'level186-{ident}.json').read_text(encoding='utf-8'))
    raw = FORMAT.pack(b'CCR1', *controller_data(read))
    output.mkdir(parents=True, exist_ok=True)
    (output / 'city-crest-controller.bin').write_bytes(raw)
    language = {e.attrib['name']: e.text for e in ET.parse(prompts).getroot().iter('entry')}
    room = room_data(read, marker, language)
    (output / 'city-crest-room.bin').write_bytes(room)
    (output / 'city-crest-audio.bin').write_bytes(struct.pack('<4s3f', b'CCA1', *audio_position(read)))
    print(f'City Crest source controller/room: {len(raw)}/{len(room)} bytes')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=SOURCE)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    convert(args.source, args.output)
