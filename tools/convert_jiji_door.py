"""Restore Town's Jiji sidecars from exported conversation, trigger and gate data."""
import converted_files  # Protect this checkout's converted assets on direct use.
import argparse
import json
import math
from pathlib import Path
import struct
import xml.etree.ElementTree as ET

from assetripper_client import ROOT
from convert_world_key_doors import room_data as controller_data, room_bytes as controller_bytes, require
from inspect_fsm import decode_fsm
from private_output_paths import is_private_output
from unity_transform import project

SOURCE = ROOT / '_private/research/crossroads-scenes/Town/terrain'
PROMPTS = ROOT / '_private/research/tutorial-reminders/EN_Prompts-decrypted.xml'
FORMAT = struct.Struct('<4s32sI7f768s768s32s32s32s')


def room_data(read, prompts):
    owner, transform, collider = (read(n) for n in (818, 2005, 2565))
    require(owner['m_Name'] == 'Jiji Door' and owner['m_IsActive'], 'Changed Jiji owner')
    components = [c['m_Component'] for c in owner['m_Components']]
    require(all(dict(m_FileID=0, m_PathID=n) in components for n in (2005, 2565, 4067, 4068)),
            'Missing Jiji components')
    require(transform['m_GameObject'] == collider['m_GameObject'] == dict(m_FileID=0, m_PathID=818)
            and transform['m_Father'] == dict(m_FileID=0, m_PathID=0), 'Unsupported Jiji transform owner/parent')
    require(collider['m_Enabled'] and collider['m_IsTrigger'], 'Jiji interaction requires active trigger')
    gate, transition = read(167), read(4407)['m_Structure']
    require(gate['m_Name'] == 'door_jiji' and gate['m_IsActive'] and transition['isADoor']
            and transition['targetScene'] == 'Room_Ouiji' and transition['entryPoint'] == 'left1',
            'Changed Jiji entrance destination')
    npc = decode_fsm(read(4068)['m_Structure']['fsm'])
    require(npc['name'] == 'npc_control', 'Changed Jiji interaction FSM')
    variables = npc['variables']
    flags = {v['name']: v['value'] for v in variables['boolVariables']}
    require(flags['Hero Always Left'] and not flags['Hero Always Right'], 'Unsupported Jiji hero approach')
    offset = next(v['value'] for v in variables['floatVariables'] if v['name'] == 'Move To Offset')
    prompt = next(v['value'] for v in variables['stringVariables'] if v['name'] == 'Prompt Name')
    require(prompt == 'Inspect', 'Changed Jiji inspect prompt')
    move = next(s for s in npc['states'] if s['name'] == 'Move Hero Left')
    velocity = [a['parameters']['x'] for a in move['actions']
                if a.get('enabled', True) and a['type'].endswith('.SetVelocity2d')]
    require(len(velocity) == 1 and not velocity[0]['useVariable'] and velocity[0]['value'] < 0,
            'Unsupported Jiji approach speed')
    speed = -velocity[0]['value']
    conversation = decode_fsm(read(4067)['m_Structure']['fsm'])
    for state, key in (('No Key', 'JIJI_DOOR_NOKEY'), ('Send Text', 'JIJI_DOOR_KEY')):
        actions = next(s for s in conversation['states'] if s['name'] == state)['actions']
        calls = [a['parameters'] for a in actions if a.get('enabled', True)
                 and a['type'].endswith('.CallMethodProper')]
        require(len(calls) == 1 and calls[0]['methodName']['value'] == 'StartConversation'
                and [v['stringValue'] for v in calls[0]['parameters']['elements']] == [key, 'Prompts'],
                'Changed Jiji dialogue reference')
    offset_xy, size = collider['m_Offset'], collider['m_Size']
    cx, cy = offset_xy['m_X'], offset_xy['m_Y']
    sx, sy = size['m_X'], size['m_Y']
    require(sx > 0 and sy > 0, 'Invalid Jiji trigger size')
    corners = [project((cx + dx*sx/2, cy + dy*sy/2, 0), [transform])
               for dx in (-1, 1) for dy in (-1, 1)]
    trigger = [min(v[0] for v in corners), min(v[1] for v in corners),
               max(v[0] for v in corners), max(v[1] for v in corners)]
    values = trigger + [project((0, 0, 0), [transform])[0], offset, speed]
    require(all(math.isfinite(v) and abs(v) <= 100000 for v in values)
            and offset >= 0 and speed > 0, 'Invalid Jiji placement')
    def text(key, capacity):
        value = prompts[key].replace('<br>', '\n').strip().encode('utf-8')
        require(0 < len(value) < capacity and b'\0' not in value, 'Invalid Jiji text: ' + key)
        return value
    return FORMAT.pack(b'JJR1', b'door_jiji', 818, *values,
                       text('JIJI_DOOR_NOKEY', 768), text('JIJI_DOOR_KEY', 768),
                       text('YES', 32), text('NO', 32), text('INSPECT', 32))


def convert(source, prompts, output):
    require(is_private_output(output), 'Jiji sidecars must remain private')
    def read(ident):
        return json.loads((source / f'level7-{ident}.json').read_text(encoding='utf-8'))
    language = {e.attrib['name']: e.text for e in ET.parse(prompts).getroot().iter('entry')}
    controller = controller_bytes(controller_data(read(4067)))
    room = room_data(read, language)
    output.mkdir(parents=True, exist_ok=True)
    (output / 'jiji-controller.bin').write_bytes(controller)
    (output / 'jiji-room.bin').write_bytes(room)
    print(f'Jiji source-backed room/controller: {len(room)}/{len(controller)} bytes')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=SOURCE)
    parser.add_argument('--prompts', type=Path, default=PROMPTS)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    convert(args.source, args.prompts, args.output)
