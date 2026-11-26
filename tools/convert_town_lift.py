"""Pack the original Town mine-lift rules, lever and native opening cage."""
import converted_files
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
from PIL import Image

from assetripper_client import ROOT
from atlas_packing import pack_pages
from convert_hero import decode_sprite
from convert_room_visuals import decode as decode_native
from convert_world_lifts import room_data as lift_data, room_bytes as lift_bytes
from convert_world_switch_visuals import bank_bytes
from inspect_fsm import decode_fsm
from private_output_paths import is_private_output
from unity_transform import project
from tool_paths import TEX3DS as TEX3DS_EXE

SOURCE = ROOT / '_private/research/crossroads-scenes/Town'
ART = ROOT / '_private/minimum-obstacles/town-art'
FEEDBACK = ROOT / '_private/minimum-obstacles/town-lever-feedback'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def native_clip(data):
    require(data['m_Name'] == 'town_mine_elev_cage0' and data['m_SampleRate'] == 30
            and not data['m_Events'] and not data['m_PositionCurves'] and not data['m_ScaleCurves']
            and not data['m_RotationCurves'], 'Unsupported source cage clip')
    curves = data['m_PPtrCurves']
    require(len(curves) == 1 and curves[0]['m_Attribute'] == 'm_Sprite'
            and curves[0]['m_Path'] == '', 'Unsupported cage sprite binding')
    keys = curves[0]['m_Curve']
    require(len(keys) == 22 and all(abs(k['m_Time']-i/30) < 1e-6 for i,k in enumerate(keys)),
            'Unsupported cage sprite cadence')
    hide = data['m_FloatCurves']
    require(len(hide) == 1 and hide[0]['m_Attribute'] == 'm_Enabled', 'Unsupported cage enabled curve')
    enabled = hide[0]['m_Curve']['m_Curve']
    require(len(enabled) == 2 and enabled[0]['m_Time'] == 0 and enabled[0]['m_Value'] == 1
            and abs(enabled[1]['m_Time']-.7) < 1e-6 and enabled[1]['m_Value'] == 0,
            'Changed source cage disappearance')
    frames = [dict(spriteId=10000+k['m_Value']['m_PathID']) for k in keys]
    return dict(name='Cage Open', fps=30, wrapMode=2, loopStart=0, frames=frames), enabled[1]['m_Time']


def lever_hit_wait(state):
    actions=[a for a in state['actions'] if a.get('enabled',True)]
    waits=[a['parameters'] for a in actions if a['type'].endswith('.Wait')]
    require(len(waits)==1 and not waits[0]['realTime'] and not waits[0]['time']['useVariable']
            and waits[0]['finishEvent']=='FINISHED', 'Unsupported Town lever wait')
    wait=waits[0]['time']['value'];require(abs(wait-.75)<1e-6, 'Changed Town lever timing')
    freezes=[a['parameters'] for a in actions if a['type'].endswith('.SendMessage')
             and a['parameters']['functionCall']['FunctionName']=='FreezeMoment']
    require(len(freezes)==1, 'Missing or ambiguous Town lever freeze')
    freeze=freezes[0];call=freeze['functionCall'];target=freeze['gameObject']
    require(target['ownerOption']==1 and target['gameObject']['useVariable']
            and target['gameObject']['name']=='GameManager' and call['parameterType']=='int'
            and not call['IntParameter']['useVariable'] and call['IntParameter']['value']==1,
            'Changed Town lever FreezeMoment(1)')
    return wait


def feedback_origin(feedback):
    report=json.loads((feedback/'sources.json').read_text())
    require(bool(report['sha256']), 'Missing Town feedback provenance')
    for name,digest in report['sha256'].items():
        path=(feedback/name).resolve()
        require(path.is_relative_to(feedback.resolve()) and
                hashlib.sha256(path.read_bytes()).hexdigest()==digest, 'Changed Town feedback source: '+name)
    effect=report['effect']
    require(effect['file']=='resources.assets-7281.json' and effect['name']=='Strike Nail R'
            and effect['parameters']['gameObject']['value']==dict(m_FileID=3,m_PathID=7281)
            and effect['parameters']['spawnPoint']['name']=='Hit Origin', 'Changed Town lever strike effect')
    origin=report['hit_origin']
    require(len(origin)==3 and all(math.isfinite(v) and abs(v)<=100000 for v in origin),
            'Invalid Town lever Hit Origin')
    return report,origin


def audio_profile(profile):
    require(profile['m_MinDistance']==39 and profile['m_MaxDistance']==50 and
            profile['m_RolloffMode']==2 and profile['m_Volume']==1 and profile['m_Pitch']==1 and
            not profile['m_Loop'] and profile['m_Pan2D']==0, 'Changed Town lever audio profile')
    blend=profile['m_PanLevelCustomCurve']['m_Curve']
    require(len(blend)==1 and blend[0]['m_Value']==1, 'Changed Town lever spatial blend')
    curve=profile['m_RolloffCustomCurve']['m_Curve']
    expected=((.78,1,0,-4.545454),(.9411765,0,-9.596051,-9.596051))
    require(len(curve)==2, 'Changed Town lever rolloff keys')
    for key,values in zip(curve,expected):
        require(key['m_WeightedMode']==0 and all(abs(key[name]-value)<1e-6 for name,value in
                zip(('m_Time','m_Value','m_InSlope','m_OutSlope'),values)), 'Changed Town lever rolloff')


def convert(source, art, output, tex3ds, feedback=FEEDBACK):
    require(is_private_output(output), 'Town assets must remain private')
    feedback_report,hit_origin=feedback_origin(feedback)
    audio_profile(feedback_report['audio_source'])
    require(hashlib.sha256((source/'terrain/level7-3919.json').read_bytes()).hexdigest()==feedback_report['scene_fsm_sha256'],
            'Changed Town lever feedback FSM')
    manifest = json.loads((art/'sources.json').read_text())
    require(manifest['scene'] == 'Town', 'Wrong Town art source')
    for name, digest in manifest['sha256'].items():
        path = (art/name).resolve()
        require(path.is_relative_to(art.resolve()) and hashlib.sha256(path.read_bytes()).hexdigest() == digest,
                'Changed original Town export: '+name)
    for name, digest in manifest['scene_sources_sha256'].items():
        require(hashlib.sha256((source/name).read_bytes()).hexdigest() == digest, 'Changed Town source: '+name)
    def read(ident):
        name=f'level7-{ident}.json'
        path=source/'terrain'/name
        if not path.exists():path=source/'visuals'/name
        return json.loads(path.read_text())
    def part(name):
        return json.loads((art/manifest['parts'][name]['file']).read_text())
    def chain(ident):
        go=read(ident)
        transforms=[read(c['m_Component']['m_PathID']) for c in go['m_Components']]
        transform=next(t for t in transforms if 'm_LocalPosition' in t)
        result=[];seen=set()
        while True:
            result.append(transform);parent=transform['m_Father']
            if not parent['m_PathID']:return result
            require(not parent['m_FileID'] and parent['m_PathID'] not in seen, 'External/cyclic Town transform')
            seen.add(parent['m_PathID']);transform=read(parent['m_PathID'])
    def trigger(ident):
        c=read(ident);transforms=chain(c['m_GameObject']['m_PathID']);offset=c['m_Offset']
        require(c['m_Enabled'] and c['m_IsTrigger'], 'Town interaction requires an enabled trigger')
        if 'm_Points' in c:
            points=[(p['m_X']+offset['m_X'],p['m_Y']+offset['m_Y'],0) for path in c['m_Points']['m_Paths'] for p in path]
        else:
            size=c['m_Size'];points=[(offset['m_X']+x*size['m_X']/2,offset['m_Y']+y*size['m_Y']/2,0)
                                   for x in (-1,1) for y in (-1,1)]
        points=[project(p,transforms) for p in points]
        return [min(p[0] for p in points),min(p[1] for p in points),max(p[0] for p in points),max(p[1] for p in points)]
    def draw(owner, renderer, scale, color):
        transforms=chain(owner);o=project((0,0,0),transforms)
        x=project((scale[0],0,0),transforms);y=project((0,scale[1],0),transforms)
        r=read(renderer)
        return [x[0]-o[0],y[0]-o[0],o[0],x[1]-o[1],y[1]-o[1],o[1],o[2],*color,
                r['m_SortingLayer'],r['m_SortingOrder'],x[2]-o[2],y[2]-o[2],0]
    controller=lift_bytes(lift_data(read(4026)))
    switch=decode_fsm(read(3919)['m_Structure']['fsm'])
    states={s['name']:s for s in switch['states']}
    wait=lever_hit_wait(states['Hit'])
    flags={v['name']:v['value'] for v in switch['variables']['boolVariables']}
    require(not flags['SetPlayerData'], 'Town lever persistence changed')
    persistent=read(4421)['m_Structure']
    require(not persistent['semiPersistent'] and not persistent['dontSave'], 'Town lever persistence disabled')
    library=part('library')['m_Structure'];collection=part('collection')['m_Structure']
    clips=[next(c for c in library['clips'] if c['name']==name) for name in ('Lever Idle','Lever Hit','Lever Activated')]
    cage,hide=native_clip(part('cage_clip'));clips.append(cage)
    sprite_ids=sorted({f['spriteId'] for c in clips for f in c['frames']})
    texture_files={Path(name).stem:art/name for name in manifest['textures']}
    texture=lambda ident:Image.open(texture_files[f'sharedassets7.assets-{ident}']).convert('RGBA')
    require(len(collection['textures'])==1, 'Town lever texture mapping changed')
    atlas=texture(collection['textures'][0]['m_PathID'])
    sprites=[]
    for ident in sprite_ids:
        if ident<10000:
            sprites.append((ident,*decode_sprite(atlas,collection['spriteDefinitions'][ident])))
        else:
            data=json.loads((art/f'sharedassets7.assets-{ident-10000}.json').read_text())
            sprites.append((ident,*decode_native(texture(data['m_RD']['m_Texture']['m_PathID']),data)))
    records,pages=pack_pages(sprites,[(1024,1024),(1024,1024)])
    output.mkdir(parents=True,exist_ok=True)
    for i,page in enumerate(pages):
        png=output/f'town-lift-page-{i}.png';page.save(png)
        subprocess.run([str(tex3ds),'-f','rgba4','-z','none','-o',str(output/f'town-lift-page-{i}.t3x'),str(png)],check=True)
    mapping=json.loads((source/'visuals/room-visuals-map.json').read_text())
    owners=[int(r['go'].removeprefix('level7-').removesuffix('.json')) for r in mapping['inventory'] if 'level7-1888.json' in r['chain']]
    require(0<len(owners)<=64 and len(set(owners))==len(owners), 'Unsupported moving Town inventory')
    initial=project((0,0,0),chain(703))[1]
    lever=read(3791)['m_Structure'];scale=lever['_scale'];color=lever['_color']
    cage_renderer=read(3513);cage_color=cage_renderer['m_Color']
    require(not cage_renderer['m_FlipX'] and not cage_renderer['m_FlipY'], 'Unsupported cage sprite flip')
    draws=[(316,draw(316,2403,(scale['x'],scale['y']),[color[c] for c in 'rgba'])),
           (687,draw(687,3513,(1,1),[cage_color['m_'+c.upper()] for c in 'rgba']))]
    values=[*trigger(2484),*trigger(2556),wait,initial,hide]
    require(all(math.isfinite(v) for v in values), 'Invalid Town sidecar numbers')
    colliders=[2488,2552,2554,2555,2557]
    raw=bytearray(struct.pack('<4s32s64s4II8II64I11f2I',b'TLR2',b'Town',b'Mines Lever',2557,2484,712,687,
                              len(colliders),*(colliders+[0]*(8-len(colliders))),len(owners),*(owners+[0]*(64-len(owners))),
                              *values,sprite_ids.index(lever['_spriteId']),sprite_ids.index(cage['frames'][0]['spriteId'])))
    for owner,values in draws:raw.extend(struct.pack('<I16f',owner,*values))
    raw.extend(struct.pack('<3f',*hit_origin))
    (output/'town-lift-controller.bin').write_bytes(controller)
    (output/'town-lift-room.bin').write_bytes(raw)
    (output/'town-lift.bin').write_bytes(bank_bytes(clips,sprite_ids,records,page_count=len(pages)))
    print(f'Town original lift: {len(sprites)} sprites, {len(pages)} pages, {len(owners)} moving owners; sidecar {len(raw)} bytes')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,default=SOURCE)
    parser.add_argument('--art',type=Path,default=ART)
    parser.add_argument('--feedback',type=Path,default=FEEDBACK)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--tex3ds',type=Path,default=Path(TEX3DS_EXE))
    args=parser.parse_args();convert(args.source,args.art,args.output,args.tex3ds,args.feedback)
