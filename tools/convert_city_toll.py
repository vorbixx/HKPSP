"""Crossroads_49b lift toll from its authored FSM and global YN debit path."""
import converted_files
import argparse
import hashlib
import json
import math
import struct
import xml.etree.ElementTree as ET
from pathlib import Path
from assetripper_client import ROOT
from inspect_fsm import decode_fsm
from convert_world_key_doors import require
from convert_jiji_door import PROMPTS
from private_output_paths import is_private_output
from unity_transform import project

SOURCE=ROOT/'_private/research/crossroads-scenes/Crossroads_49b/terrain'
DIALOGUE=ROOT/'_private/research/lore-tablets'
PRICES=ROOT/'_private/research/stag-stations/EN_Prices-decrypted.xml'  # decrypted from resources.assets by extract_stag.py
FORMAT=struct.Struct('<4s9f2I64s768s64s32s32s32s')

def one(fsm,state,kind):
    s=next(s for s in fsm['states'] if s['name']==state)
    rows=[a['parameters'] for a in s['actions'] if a.get('enabled',True) and a['type'].endswith('.'+kind)]
    require(len(rows)==1,'Unsupported City toll '+state+'/'+kind)
    return rows[0]

def room_data(read,prompts,prices,yn,enough):
    fsm=decode_fsm(read(819)['m_Structure']['fsm'])
    strings={v['name']:v['value'] for v in fsm['variables']['stringVariables']}
    flag=strings['PlayerData Bool'];price_key=strings['Price String Name']
    require(flag=='cityLift1' and price_key=='CITY_LIFT_1','Changed City toll identity')
    require(one(fsm,'Paid?','PlayerDataBoolTest')['boolName']['value']==flag,'Changed paid test')
    write=one(fsm,'Send Message','SetPlayerDataBool')
    require(write['boolName']['value']==flag and write['value']['value'] is True,'Changed City unlock')
    require(one(fsm,'Send Message','SendEventByName')['sendEvent']['value']=='TOLL LIFT PAY','Changed lift pay event')
    require(one(fsm,'Get Price','GetLanguageString')['sheetName']['value']=='Prices' and
            one(fsm,'Get Price','ConvertStringToInt')['intVariable']==dict(variable='Toll Cost'),'Changed price lookup')
    cost=int(prices[price_key]);require(0<cost<=9999999,'Invalid City toll price')
    call=one(fsm,'Send Text','CallMethodProper')
    require(call['methodName']['value']=='StartConversation' and
            [p['stringValue'] for p in call['parameters']['elements']]==['TOLLBOOTH_LIFT','Prompts'],'Changed toll text')
    require(one(fsm,'Send Text','SetFsmInt')['setValue']==dict(variable='Toll Cost'),'Changed YN cost forwarding')
    debit=one(yn,'Take Geo','CallMethodProper')
    require(debit['behaviour']['value']=='HeroController' and debit['methodName']['value']=='TakeGeo' and
            debit['parameters']['elements'][0]['variableName']=='Toll Cost','Changed global Geo debit')
    threshold=one(enough,'Init','IntCompare')
    require(threshold['integer1']==dict(variable='Toll Cost') and threshold['integer2']==dict(variable='Geo') and
            threshold['greaterThan']=='NOT ENOUGH','Changed affordability test')
    owner,transform,collider=read(29),read(294),read(559)
    require(owner['m_Name']=='Toll Machine Lift' and owner['m_IsActive'] and collider['m_IsTrigger'] and
            collider['m_Enabled'] and collider['m_GameObject']['m_PathID']==29 and
            transform['m_Father']['m_PathID']==0,'Changed City toll placement')
    offset,size=collider['m_Offset'],collider['m_Size']
    points=[project((offset['m_X']+dx*size['m_X']/2,offset['m_Y']+dy*size['m_Y']/2,0),[transform])
            for dx in (-1,1) for dy in (-1,1)]
    trigger=[min(p[0] for p in points),min(p[1] for p in points),max(p[0] for p in points),max(p[1] for p in points)]
    floats={v['name']:v['value'] for v in fsm['variables']['floatVariables']}
    speeds=[one(fsm,s,'SetVelocity2d')['x']['value']*sign for s,sign in [('Move Hero Left',-1),('Move Hero Right',1)]]
    require(speeds[0]==speeds[1] and speeds[0]>0,'Changed City approach')
    waits=[one(fsm,s,'Wait') for s in ('Box Up','Pause Before Box Drop')]
    require(all(not w['realTime'] and not w['time']['useVariable'] and w['finishEvent']=='FINISHED' for w in waits),'Changed toll wait')
    values=trigger+[project((0,0,0),[transform])[0],floats['Move To Offset'],speeds[0]]+[w['time']['value'] for w in waits]
    require(all(math.isfinite(v) for v in values),'Nonfinite City toll')
    def text(key,n):
        value=prompts[key].replace('<br>','\n').strip()
        if key=='TOLLBOOTH_LIFT':value=value.replace('. Insert','.\nInsert')
        raw=value.encode('utf-8')
        require(0<len(raw)<n and b'\0' not in raw,'Invalid toll text '+key);return raw
    return FORMAT.pack(b'CTF1',*values,cost,29,flag.encode(),text('TOLLBOOTH_LIFT',768),text('NOT_ENOUGH_GEO',64),
                       text('YES',32),text('NO',32),text('INSPECT',32))

def convert(source,dialogue,prompts,prices,output):
    require(is_private_output(output),'City toll output must remain private')
    hashes={}
    def read_path(path):
        raw=path.read_bytes();hashes[str(path)]=hashlib.sha256(raw).hexdigest();return json.loads(raw)
    def read(ident):return read_path(source/f'level80-{ident}.json')
    def language(path):
        raw=path.read_bytes();hashes[str(path)]=hashlib.sha256(raw).hexdigest()
        return {e.attrib['name']:e.text for e in ET.fromstring(raw).iter('entry')}
    yn=decode_fsm(read_path(dialogue/'resources.assets-21856.json')['m_Structure']['fsm'])
    enough=decode_fsm(read_path(dialogue/'resources.assets-22339.json')['m_Structure']['fsm'])
    raw=room_data(read,language(prompts),language(prices),yn,enough)
    output.mkdir(parents=True,exist_ok=True);(output/'city-toll.bin').write_bytes(raw)
    (output/'city-toll-source.json').write_text(json.dumps(dict(source_sha256=hashes),indent=2)+'\n')
    print('City lift toll: source price150, global affordability/debit, persisted cityLift1;',len(raw),'bytes')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name,default in [('source',SOURCE),('dialogue',DIALOGUE),('prompts',PROMPTS),('prices',PRICES)]:p.add_argument('--'+name,type=Path,default=default)
    p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    convert(a.source,a.dialogue,a.prompts,a.prices,a.output)
