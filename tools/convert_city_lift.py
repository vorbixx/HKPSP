"""Crossroads_49b toll lift lowering, from FSM845/851 and authored hierarchy."""
import converted_files
import argparse
import hashlib
import json
import math
import struct
from pathlib import Path
from assetripper_client import ROOT
from inspect_fsm import decode_fsm
from convert_city_toll import SOURCE, one
from convert_world_key_doors import require
from private_output_paths import is_private_output

PROFILES={'Crossroads_49b':(80,845,851,478,215),'Crossroads_49':(79,1100,1106,472,123)}

def lift_data(read,terrain,visuals):
    level,lower_id,move_id,root_id,owner=PROFILES[terrain['scene']]
    root_name=f'level{level}-{owner}.json'
    fsm=decode_fsm(read(lower_id)['m_Structure']['fsm'])
    move=decode_fsm(read(move_id)['m_Structure']['fsm'])
    require(next(v['value'] for v in move['variables']['stringVariables'] if v['name']=='PlayerData Bool')=='cityLift1',
            'Changed City lift save identity')
    transform=read(root_id)
    require(transform['m_GameObject']['m_PathID']==owner and read(owner)['m_Name']=='elev_main','Changed City lift root')
    if transform['m_Father']['m_PathID']:
        parent=read(transform['m_Father']['m_PathID'])
        require(not parent['m_Father']['m_PathID'] and parent['m_LocalPosition']==dict(m_X=0,m_Y=0,m_Z=0) and
                parent['m_LocalScale']==dict(m_X=1,m_Y=1,m_Z=1) and parent['m_LocalRotation']==dict(m_X=0,m_Y=0,m_Z=0,m_W=1),
                'City lift parent requires explicit world conversion')
    state=next(s for s in fsm['states'] if s['name']=='Up')
    require(any(t['fsmEvent']['name']=='TOLL LIFT PAY' and t['toState']=='Come Down' for t in state['transitions']),
            'Changed City lift pay admission')
    tween=one(fsm,'Come Down','iTweenMoveTo')
    # iTween.cs EaseType.easeOutCubic=4 and easeOutCubic(), recovered via rea_query.
    require(tween['easeType']==4 and tween['space']==0 and not tween['realTime']['value'] and tween['loopType']==0 and
            tween['finishEvent']=='FINISHED' and tween['vectorPosition']==dict(variable='Lower Vector') and
            tween['speed']['value']==0,'Unsupported City lift tween')
    bottom=next(v['value'] for v in fsm['variables']['floatVariables'] if v['name']=='Bottom Y')
    require(bottom==next(v['value'] for v in move['variables']['floatVariables'] if v['name']=='Bottom Y'),
            'City lift FSM endpoints disagree')
    wait=one(fsm,'Land','Wait');require(not wait['realTime'] and wait['finishEvent']=='FINISHED','Changed City landing wait')
    values=[transform['m_LocalPosition']['m_Y'],bottom,tween['delay']['value'],tween['time']['value'],wait['time']['value']]
    require(all(math.isfinite(v) for v in values) and values[0]>=values[1] and values[2]>=0 and values[3]>0 and values[4]>=0,
            'Invalid City lift motion')
    variables={v['name']:v['value']['m_PathID'] for v in fsm['variables']['gameObjectVariables']}
    def block(name):
        ident=variables[name];require(read(ident)['m_Name']==name,'Changed City block identity')
        rows=[r for r in terrain['inventory'] if r['go']==f'level{level}-{ident}.json']
        return [int(Path(r['collider']).stem.rsplit('-',1)[1]) for r in rows if read(int(Path(r['collider']).stem.rsplit('-',1)[1]))['m_Enabled']]
    opened,closed,inactive=block('Open Block'),block('Closed Block'),block('Inactive Block')
    require(len(opened)==3 and len(closed)==3 and len(inactive)==1,'Changed City block collider counts')
    colliders=[]
    for row in terrain['inventory']:
        if root_name not in row['owners']:continue
        ident=int(Path(row['collider']).stem.rsplit('-',1)[1]);c=read(ident)
        if c['m_Enabled'] and not c['m_IsTrigger']:colliders.append(ident)
    # tk2d Door L/R are in mesh_renderer_audit, outside the native inventory;
    # convert_city_lift_art validates both parent transforms before admission.
    doors={int(Path(r['go']).stem.rsplit('-',1)[1]) for r in visuals['mesh_renderer_audit'] if r.get('name') in ('Door L','Door R','Lift Call Lever')}
    require(len(doors)==3,'Changed City door/lever owners')
    owners=sorted(doors|{int(Path(r['go']).stem.rsplit('-',1)[1]) for r in visuals['inventory'] if root_name in r['owners']})
    require(len(colliders)==12 and len(set(colliders))==12 and 0<len(owners)<=96 and
            set(opened+closed+inactive)<=set(colliders),'Changed City lift hierarchy')
    return struct.pack('<4s5f3I6I','CLD1'.encode(),*values,len(colliders),len(owners),inactive[0],*opened,*closed)+\
        struct.pack('<'+str(len(colliders)+len(owners))+'I',*colliders,*owners)

def convert(source,output):
    require(is_private_output(output),'City lift output must remain private')
    hashes={}
    def read_path(path):
        raw=path.read_bytes();hashes[str(path)]=hashlib.sha256(raw).hexdigest();return json.loads(raw)
    level=PROFILES[source.parent.name][0]
    def read(ident):
        path=source/f'level{level}-{ident}.json'
        if not path.exists():path=source.parent/'visuals'/path.name
        return read_path(path)
    raw=lift_data(read,read_path(source/'terrain-map.json'),read_path(source.parent/'visuals/room-visuals-map.json'))
    output.mkdir(parents=True,exist_ok=True);(output/'city-lift.bin').write_bytes(raw)
    (output/'city-lift-source.json').write_text(json.dumps(dict(source_sha256=hashes),indent=2)+'\n')
    print('City lift: source delayed cubic lower, paid restore and collision blocks;',len(raw),'bytes')

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,default=SOURCE)
    parser.add_argument('--output',type=Path,default=ROOT/'_private/converted/scenes/Crossroads_49b')
    args=parser.parse_args();convert(args.source,args.output)
