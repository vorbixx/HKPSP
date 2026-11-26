"""Source lift FSM851/1106 travel rules, input admission and arrival pairing."""
import converted_files
import argparse
import hashlib
import json
import math
import struct
from pathlib import Path
from assetripper_client import ROOT
from inspect_fsm import decode_fsm
from convert_city_toll import one
from convert_world_key_doors import require
from private_output_paths import is_private_output
from unity_transform import project

PROFILES={'Crossroads_49b':(80,851,847,478,580,573),'Crossroads_49':(79,1106,1102,472,749,741)}

def transport_data(read,scene):
    level,move_id,lever_id,root_id,lever_shape,hero_shape=PROFILES[scene]
    move=decode_fsm(read(move_id)['m_Structure']['fsm'])
    lever=decode_fsm(read(lever_id)['m_Structure']['fsm'])
    strings={v['name']:v['value'] for v in move['variables']['stringVariables']}
    require(one(lever,'Send Msg','SendEventByName')['sendEvent']['value']=='MOVE' and
            one(lever,'Send Msg','BoolTest')['boolVariable']==dict(variable='Hero Check'),'Changed City lever admission')
    target=one(move,'New Scene','BeginSceneTransition')
    require(target['sceneName']['useVariable'] and target['sceneName']['name']=='New Scene','Changed City scene binding')
    destination=strings[target['sceneName']['name']];entry=target['entryGateName']['value']
    require(destination==('Crossroads_49' if scene=='Crossroads_49b' else 'Crossroads_49b') and entry=='elev_entrance',
            'Changed City transport pair')
    require(strings['PlayerData Bool']=='cityLift1','Changed City transport unlock')
    require(one(move,'New Scene','SetPlayerDataBool')['boolName']['value']=='liftArrival' and
            one(move,'Set Hero pos','SetPlayerDataBool')['boolName']['value']=='liftArrival','Changed City arrival flag')
    variables={v['name']:v['value'] for kind in ('floatVariables','boolVariables','vector3Variables') for v in move['variables'][kind]}
    departure=one(move,'Move Out','iTweenMoveBy');arrival=one(move,'Arrival Tween','iTweenMoveTo')
    require(departure['easeType']==3 and arrival['easeType']==4 and departure['speed']['value']>0 and arrival['speed']['value']>0 and
            not departure['speed']['useVariable'] and not arrival['speed']['useVariable'],'Changed City transport tween')
    move_vector=variables['Move Vector'];hero_vector=variables['Hero Set Vector']
    require(move_vector['x']==0 and move_vector['z']==0 and abs(move_vector['y'])==30,'Changed City departure vector')
    root=read(root_id)
    require(root['m_LocalScale']==dict(m_X=1,m_Y=1,m_Z=1),'City transport requires source unit scale')
    def chain(ident):
        go=read(ident);t=next(read(c['m_Component']['m_PathID']) for c in go['m_Components'] if 'm_LocalPosition' in read(c['m_Component']['m_PathID']))
        result=[t];seen=set()
        while t['m_Father']['m_PathID']:
            parent=t['m_Father'];require(not parent['m_FileID'] and parent['m_PathID'] not in seen,'Cyclic/external City trigger')
            seen.add(parent['m_PathID']);t=read(parent['m_PathID']);result.append(t)
        return result
    def trigger(ident):
        c=read(ident);require(c['m_Enabled'] and c['m_IsTrigger'],'City lever/check requires source trigger')
        ts=chain(c['m_GameObject']['m_PathID']);offset=c['m_Offset'];size=c['m_Size']
        points=[project((offset['m_X']+x*size['m_X']/2,offset['m_Y']+y*size['m_Y']/2,0),ts) for x in (-1,1) for y in (-1,1)]
        return [min(p[0] for p in points),min(p[1] for p in points),max(p[0] for p in points),max(p[1] for p in points)]
    # Speed overrides the serialized time in iTweenMoveBy.DoiTween/MoveTo.DoiTween.
    # iTween GenerateMoveByTargets computes duration=distance/speed. The tween
    # remains active during Fade because the source stopOnExit is false.
    close=one(move,'Close','Wait')['time']['value'];out=one(move,'Move Out','Wait')['time']['value'];fade=one(move,'Fade','Wait')['time']['value']
    fade_time=one(move,'Fade','SetFsmFloat')['setValue']['value'];land=one(move,'Arrive','Wait')['time']['value']
    arrives_up=variables['Arrives Up']
    offset=one(move,'Set Down' if arrives_up else 'Set Up','Translate')['y']['value']
    floats=[*trigger(lever_shape),*trigger(hero_shape),root['m_LocalPosition']['m_X'],root['m_LocalPosition']['m_Y'],variables['Bottom Y'],
            hero_vector['x'],hero_vector['y'],close,move_vector['y'],departure['speed']['value'],out,fade,fade_time,offset,arrival['speed']['value'],land]
    require(len(floats)==22 and all(math.isfinite(v) for v in floats),'Invalid City transport rules')
    def text(s):require(0<len(s)<32,'Invalid City transport string');return s.encode().ljust(32,b'\0')
    return struct.pack('<4s22f32s32s',b'CTP1',*floats,text(destination),text(entry))

def convert(source,output):
    require(is_private_output(output),'City transport output must remain private')
    scene=source.parent.name;level=PROFILES[scene][0];hashes={}
    def read(n):
        path=source/f'level{level}-{n}.json'
        if not path.exists():path=source.parent/'visuals'/path.name
        raw=path.read_bytes();hashes[str(path)]=hashlib.sha256(raw).hexdigest();return json.loads(raw)
    raw=transport_data(read,scene);output.mkdir(parents=True,exist_ok=True);(output/'city-transport.bin').write_bytes(raw)
    (output/'city-transport-source.json').write_text(json.dumps(dict(source_sha256=hashes),indent=2)+'\n')
    print('City transport',scene,len(raw),'bytes; source lever/Hero Check, speed precedence,paired arrival')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    args=p.parse_args();convert(args.source,args.output)
