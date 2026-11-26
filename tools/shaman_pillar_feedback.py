"""Source-only Crossroads_06 particle and PCM admission; reused pillar atlas."""
import copy
import hashlib
import json
import math
import struct
import wave
from PIL import Image
from assetripper_client import ROOT
RECORD=struct.Struct('<6I89f')
def canonical(value):
    if isinstance(value,list):return [canonical(v) for v in value]
    if not isinstance(value,dict):return value
    return {(k if k.startswith('m_') else 'm_'+k[0].upper()+k[1:]):canonical(v) for k,v in value.items()}
from convert_decor_effects import limits, particle_alpha
from convert_room_visuals import project
from scene_lighting import scene_ambient

SOURCE=ROOT/'_private/minimum-obstacles/shaman-pillar-feedback'
EMITTERS=((122,2397,2467),(119,2398,2465),(123,2399,2468),(118,2400,2466),(121,2401,2469),(965,2448,2513))
RATES=(90,75,100,6,41.9,3)
CAPS=(20,17,22,4,10,6)

def require(ok,message):
    if not ok: raise ValueError(message)

def compile_feedback(first_sprite, source=SOURCE):
    report=json.loads((source/'source-report.json').read_text())
    read=lambda n: json.loads((source/f'level42-{n}.json').read_text())
    data=bytearray(struct.pack('<4sI',b'SPF1',6));sprites=[];clips=[];records=[]
    for index,(owner,ident,rid) in enumerate(EMITTERS):
        raw=read(ident);p=canonical(raw);initial=p['m_InitialModule'];shape=p['m_ShapeModule'];uv=p['m_UVModule']
        require(not p['m_Looping'] and not p['m_PlayOnAwake'] and not p['m_Prewarm'] and
                p['m_MoveWithTransform']==(1 if index in (3,5) else 0) and
                shape['m_Type']==(12 if index==3 else 5) and not initial['m_Size3D'] and
                initial['m_StartColor']['m_MinMaxState']==0 and
                not any(p['m_'+k]['m_Enabled'] for k in ('ForceModule','VelocityModule','ClampVelocityModule',
                    'InheritVelocityModule','ExternalForcesModule','NoiseModule','CollisionModule','SubModule',
                    'SizeBySpeedModule','ColorBySpeedModule','LightsModule','TrailModule')),
                'Unsupported pillar particle module')
        rate=limits(p['m_EmissionModule']['m_RateOverTime'])
        require(all(abs(v-RATES[index])<.001 for v in rate) and not p['m_EmissionModule']['m_Bursts'], 'Changed pillar emission')
        renderer=read(rid);row=next(c for c in report['children'] if c['game_object']==owner)
        transform=next(c['id'] for c in row['components'] if c['type']=='Transform')
        chain=[canonical(read(transform))]
        while chain[-1]['m_Father']['m_PathID']:
            require(len(chain)<32,'Invalid particle hierarchy')
            chain.append(canonical(read(chain[-1]['m_Father']['m_PathID'])))
        origin=project((0,0,0),chain);rotation=copy.deepcopy(chain)
        for tr in rotation:
            tr['m_LocalPosition']=dict(m_X=0,m_Y=0,m_Z=0);tr['m_LocalScale']=dict(m_X=1,m_Y=1,m_Z=1)
        axes=[];directions=[]
        for axis in range(3):
            point=[0,0,0];point[axis]=1
            axes.extend(a-b for a,b in zip(project(point,chain),origin));directions.extend(project(point,rotation))
        v=[p['m_LengthInSec'],limits(p['m_StartDelay'])[1]]
        for field in ('m_StartLifetime','m_StartSpeed','m_StartSize','m_StartRotation'):v.extend(limits(initial[field]))
        v += [limits(initial['m_GravityModifier'])[1],shape['m_Radius']['m_Value'],0,math.radians(shape['m_Arc']['m_Value']),0,1e6,*origin[:2],*axes,*directions]
        v += [0]*12
        spin=p['m_RotationBySpeedModule'] if index==5 else p['m_RotationModule']
        v += limits(spin['m_Curve']) if spin['m_Enabled'] else [0,0]
        for side in (0,1):v += [limits(p['m_SizeModule']['m_Curve'],i/7)[side] if p['m_SizeModule']['m_Enabled'] else 1 for i in range(8)]
        # approximation: eight-sample source curves and shared ambient Sprites/Lit tint, as City dust.
        v += [particle_alpha(p['m_ColorModule'],i/7) for i in range(8)]
        v += [RATES[index]]*8+[shape['m_Scale']['m_'+a] for a in 'XYZ']+[shape['m_Length']]
        require(len(v)==86 and all(math.isfinite(x) for x in v),'Invalid pillar particle record')
        material_id,texture_id=(170,248) if index==5 else (162,438) if index==3 else (146,371)
        require(renderer['m_RenderMode']==0 and renderer['m_Materials']==[dict(m_FileID=5,m_PathID=material_id),dict(m_FileID=0,m_PathID=0)],'Changed pillar billboard/material')
        material=json.loads((source/f'resources-{material_id}.json').read_text())
        tint=dict(material['m_SavedProperties']['m_Colors'])['_Color'];start=raw['InitialModule']['startColor']['maxColor'];ambient=scene_ambient()
        rgba=[tint[c]*start[c]*(ambient[i] if i<3 else 1) for i,c in enumerate('rgba')]
        image=Image.open(source/f'resources-{texture_id}.png').convert('RGBA')
        columns,rows=(uv['m_TilesX'],uv['m_TilesY']) if uv['m_Enabled'] else (1,1)
        require((columns,rows)==((1,4) if index==5 else (7,2) if index==3 else (1,1)), 'Changed pillar UV layout')
        if index==5:
            require(spin['m_Enabled'] and not spin['m_SeparateAxes'] and spin['m_Range']==dict(m_X=0,m_Y=1),'Changed pillar rock rotation range')
        if index in (3,5):
            require(uv['m_Mode']==0 and uv['m_TimeMode']==0 and uv['m_Cycles']==1 and
                    uv['m_AnimationType']==(1 if index==5 else 0) and uv['m_RowMode']==1,'Unsupported pillar UV animation')
        first=first_sprite+len(sprites)
        for row_index in range(rows):
            for col in range(columns):
                bitmap=image.crop((col*image.width//columns,row_index*image.height//rows,(col+1)*image.width//columns,(row_index+1)*image.height//rows))
                bitmap=Image.merge('RGBA',tuple(ch.point(lambda x,c=c:round(x*max(0,min(1,rgba[c])))) for c,ch in enumerate(bitmap.split())))
                sprites.append((first_sprite+len(sprites),bitmap,(-.5,-.5,.5,.5)))
        count=len(sprites)+first_sprite-first
        clips.append(dict(name=f'Pillar dust {owner}',fps=1,wrapMode=0,loopStart=0,frames=[dict(spriteId=i) for i in range(first,first+count)]))
        flags=16 if index==5 else 0
        data += RECORD.pack(owner,shape['m_Type'],first,count,flags,CAPS[index],*v,origin[2],renderer['m_SortingLayer'],renderer['m_SortingOrder'])
        records.append(dict(owner=owner,origin=origin,capacity=CAPS[index]))
    return data,sprites,clips,records

def write_audio(output,source=SOURCE):
    report=json.loads((source/'source-report.json').read_text());total=0
    for role,name,collection,ident,channels,frames in (
        ('shake','switch_gate_gate','sharedassets39.assets',17,2,158588),
        ('finish','platform_dirt_fall_1','sharedassets6.assets',170,1,60772)):
        entry=next(c for c in report['clips'] if c['role']==role);path=source/f'{role}.wav';raw=path.read_bytes()
        require((entry['name'],entry['collection'],entry['path_id'])==(name,collection,ident) and hashlib.sha256(raw).hexdigest()==entry['sha256'],'Changed pillar source clip')
        with wave.open(str(path)) as wav:
            require((wav.getnchannels(),wav.getsampwidth(),wav.getframerate(),wav.getnframes(),wav.getcomptype())==(channels,2,44100,frames,'NONE'),'Unsupported pillar PCM')
            require(len(wav.readframes(frames))==frames*channels*2,'Truncated pillar PCM')
        total+=frames*channels*2;(output/f'shaman-pillar-{role}.wav').write_bytes(raw)
    require(total==755896,'Changed pillar PCM budget')
