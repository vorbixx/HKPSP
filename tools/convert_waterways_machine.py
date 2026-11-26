"""Restore Ruins1_05b's authored simple-key transaction and interaction data."""
import converted_files
import argparse
import json
import math
from pathlib import Path
import struct
import xml.etree.ElementTree as ET

from assetripper_client import ROOT
from convert_jiji_door import PROMPTS
from convert_waterways_art import ART, source_art
from convert_world_key_doors import require
from inspect_fsm import decode_fsm
from private_output_paths import is_private_output
from unity_transform import project

SOURCE = ROOT / '_private/research/crossroads-scenes/Ruins1_05b/visuals'
FORMAT = struct.Struct('<4s6f64s64s7f4I768s768s32s32s32s4I')


def controller_data(fsm):
    states = {s['name']:s for s in fsm['states']}
    def actions(state, kind):
        return [a['parameters'] for a in states[state]['actions']
                if a.get('enabled',True) and a['type'].endswith('.'+kind)]
    def one(state, kind):
        values = actions(state,kind);require(len(values) == 1, 'Unsupported Waterways '+state+'/'+kind)
        return values[0]
    key = one('Key?','GetPlayerDataInt')
    require(key['intName']['value'] == 'simpleKeys' and key['storeValue']['variable'] == 'Simple Keys',
            'Unsupported Waterways key check')
    compare = one('Key?','IntCompare')
    require(compare['integer1'] == dict(variable='Simple Keys') and compare['integer2'] == dict(value=1,useVariable=False)
            and (compare['equal'],compare['lessThan'],compare['greaterThan']) == ('KEY','NOKEY','KEY')
            and not compare['everyFrame'], 'Unsupported Waterways key threshold')
    operation = one('Activate','IntOperator')
    require(operation == dict(integer1=dict(variable='Simple Keys'),integer2=dict(value=1,useVariable=False),
                              operation=1,storeResult=dict(variable='Simple Keys'),everyFrame=False), 'Unsupported Waterways key charge')
    charge = one('Activate','SetPlayerDataInt');flag = one('Activate','SetPlayerDataBool')
    require(charge['intName']['value'] == 'simpleKeys' and charge['value'] == dict(variable='Simple Keys')
            and flag['boolName']['value'] == 'openedWaterwaysManhole' and flag['value']['value'] is True,
            'Unsupported Waterways persisted unlock')
    require(one('Open','SendEventByName')['sendEvent']['value'] == 'MANHOLE OPEN', 'Changed Waterways open event')
    waits = []
    for name in ('Box Up','Box Up YN','Decline Pause','Box Down','Yes','Activate'):
        wait = one(name,'Wait')
        event = 'TALK FINISH' if name == 'Decline Pause' else 'FINISHED'
        require(not wait['realTime'] and not wait['time']['useVariable'] and wait['finishEvent'] == event,
                'Unsupported Waterways wait')
        value = wait['time']['value'];require(math.isfinite(value) and value >= 0, 'Invalid Waterways wait')
        waits.append(value)
    for state,key in (('No Key','WATERWAY_DOOR_NOKEY'),('Send Text','WATERWAY_DOOR_KEY')):
        call = one(state,'CallMethodProper')
        require(call['methodName']['value'] == 'StartConversation' and
                [p['stringValue'] for p in call['parameters']['elements']] == [key,'Prompts'],
                'Changed Waterways original prompt')
    return waits


def room_data(read, prompts, sprites):
    fsm = decode_fsm(read(2773)['m_Structure']['fsm']);waits = controller_data(fsm)
    machine,manhole = read(1807),read(1800)
    require(machine['m_Enabled'] and machine['m_IsTrigger'] and machine['m_GameObject']['m_PathID'] == 238
            and manhole['m_Enabled'] and not manhole['m_IsTrigger'] and manhole['m_GameObject']['m_PathID'] == 147,
            'Waterways interaction trigger and removable solid must remain distinct')
    def chain(owner):
        go = read(owner);require(go['m_Name'], 'Missing Waterways owner')
        transforms = [read(c['m_Component']['m_PathID']) for c in go['m_Components']]
        t = next(t for t in transforms if 'm_LocalPosition' in t)
        result=[];seen=set()
        while True:
            result.append(t);ref=t['m_Father']
            if not ref['m_PathID']:return result
            require(not ref['m_FileID'] and ref['m_PathID'] not in seen, 'External/cyclic Waterways transform')
            seen.add(ref['m_PathID']);t=read(ref['m_PathID'])
    transforms = chain(238);offset,size = machine['m_Offset'],machine['m_Size']
    points = [project((offset['m_X']+x*size['m_X']/2,offset['m_Y']+y*size['m_Y']/2,0),transforms)
              for x in (-1,1) for y in (-1,1)]
    trigger = [min(p[0] for p in points),min(p[1] for p in points),max(p[0] for p in points),max(p[1] for p in points)]
    npc = decode_fsm(read(2775)['m_Structure']['fsm'])
    variables=npc['variables'];flags={v['name']:v['value'] for v in variables['boolVariables']}
    require(not flags['Hero Always Left'] and not flags['Hero Always Right'], 'Unsupported Waterways approach side')
    approach = next(v['value'] for v in variables['floatVariables'] if v['name'] == 'Move To Offset')
    speeds=[]
    for state,sign in (('Move Hero Left',-1),('Move Hero Right',1)):
        s=next(s for s in npc['states'] if s['name']==state)
        velocity=[a['parameters']['x'] for a in s['actions'] if a.get('enabled',True) and a['type'].endswith('.SetVelocity2d')]
        require(len(velocity)==1 and not velocity[0]['useVariable'] and velocity[0]['value']*sign>0, 'Unsupported Waterways walk')
        speeds.append(velocity[0]['value']*sign)
    require(speeds[0]==speeds[1], 'Asymmetric Waterways approach requires an adapter')
    values=trigger+[project((0,0,0),transforms)[0],approach,speeds[0]]
    require(all(math.isfinite(v) for v in values) and approach>=0, 'Invalid Waterways placement')
    def text(key,n):
        value=prompts[key].replace('<br>','\n').strip().encode('utf-8')
        require(0<len(value)<n and b'\0' not in value,'Invalid Waterways text: '+key);return value
    raw=bytearray(FORMAT.pack(b'WWM1',*waits,b'simpleKeys',b'openedWaterwaysManhole',*values,238,147,1807,1800,
                             text('WATERWAY_DOOR_NOKEY',768),text('WATERWAY_DOOR_KEY',768),
                             text('YES',32),text('NO',32),text('INSPECT',32),*sprites))
    for owner,renderer in ((237,2152),(146,2091),(236,2151),(148,2089)):
        transforms=chain(owner);r=read(renderer);c=r['m_Color']
        require(not r['m_FlipX'] and not r['m_FlipY'], 'Unsupported Waterways sprite flip')
        o=project((0,0,0),transforms);x=project((1,0,0),transforms);y=project((0,1,0),transforms)
        draw=[x[0]-o[0],y[0]-o[0],o[0],x[1]-o[1],y[1]-o[1],o[1],o[2],
              *[c['m_'+channel] for channel in 'RGBA'],r['m_SortingLayer'],r['m_SortingOrder'],x[2]-o[2],y[2]-o[2],0]
        raw.extend(struct.pack('<I16f',owner,*draw))
    return bytes(raw)


def convert(source, art, prompts, output):
    require(is_private_output(output),'Waterways sidecar must remain private')
    _,_,_,sprites=source_art(art)
    def read(ident):return json.loads((source/f'level93-{ident}.json').read_text())
    language={e.attrib['name']:e.text for e in ET.parse(prompts).getroot().iter('entry')}
    data=room_data(read,language,sprites);output.mkdir(parents=True,exist_ok=True)
    (output/'waterways-machine.bin').write_bytes(data)
    print('Waterways authored key-machine sidecar:',len(data),'bytes')


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,default=SOURCE)
    parser.add_argument('--art',type=Path,default=ART)
    parser.add_argument('--prompts',type=Path,default=PROMPTS)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();convert(args.source,args.art,args.prompts,args.output)
