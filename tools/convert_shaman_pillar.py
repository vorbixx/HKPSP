"""Crossroads_06 Switch Control3406 and Raising Pillar3351, using original art."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess

import converted_files
from assetripper_client import ROOT
from atlas_packing import pack_pages
from convert_hero import decode_sprite
from convert_room_visuals import project
from convert_world_switch_visuals import bank_bytes
from inspect_fsm import decode_fsm
from shaman_pillar_feedback import compile_feedback, write_audio
from private_output_paths import is_private_output
from tool_paths import TEX3DS as TEX3DS_EXE

SOURCE = ROOT/'_private/research/crossroads-scenes/Crossroads_06/terrain'
ART = ROOT/'_private/minimum-obstacles/switch-art'
NAMES = ('Switch Idle', 'Switch Hit', 'Switch Activated', 'Pillar Shake', 'Pillar Open', 'Pillar Opened')

def require(ok, message):
    if not ok:
        raise ValueError(message)

def source_data(source=SOURCE):
    hashes = {}
    def read(n):
        name = f'level42-{n}.json'
        raw = (source/name).read_bytes()
        hashes[name] = hashlib.sha256(raw).hexdigest()
        return json.loads(raw)
    switch, pillar = [decode_fsm(read(n)['m_Structure']['fsm']) for n in (3406, 3351)]
    def action(fsm, state, kind):
        rows = [a['parameters'] for s in fsm['states'] if s['name']==state for a in s['actions']
                if a['enabled'] and a['type'].split('.')[-1]==kind]
        require(len(rows)==1, 'Missing/ambiguous original action: '+state+'/'+kind)
        return rows[0]
    def variable(fsm, group, name):
        return next(v['value'] for v in fsm['variables'][group] if v['name']==name)
    require(variable(switch,'boolVariables','SetPlayerData') is True and
            variable(switch,'stringVariables','Player Data')=='shamanPillar' and
            variable(switch,'gameObjectVariables','Target')==dict(m_FileID=0,m_PathID=120), 'Changed shaman switch outcome')
    audio=action(switch,'Hit','AudioPlayerOneShotSingle')
    require(audio['audioClip']['value']==dict(m_FileID=9,m_PathID=106) and
            audio['audioPlayer']['value']==dict(m_FileID=5,m_PathID=4126) and audio['spawnPoint']['name']=='Self' and
            all(not audio[k]['useVariable'] and audio[k]['value']==v for k,v in
                (('pitchMin',1),('pitchMax',1),('volume',1),('delay',0))), 'Changed original switch sound')
    shake=action(switch,'Hit','SendEventByName')
    require(shake['sendEvent']['value']=='EnemyKillShake' and
            shake['eventTarget']['fsmName']['value']=='CameraShake' and shake['delay']['value']==0,
            'Changed original pillar hit camera event')
    times=[]
    for fsm,state in ((switch,'Hit'),(pillar,'Shake'),(pillar,'Finish Opening')):
        wait=action(fsm,state,'Wait')
        require(not wait['realTime'] and not wait['time']['useVariable'], 'Unsupported source clock')
        times.append(wait['time']['value'])
    require(times==[.75,.75,.5], 'Changed source waits')
    for state,event in (('Open','ACTIVATE'),('Activated','ACTIVATE AGAIN')):
        require(action(switch,state,'SendEventByName')['sendEvent']['value']==event, 'Changed target event')
    for ident in (3699,3700):
        p=read(ident)['m_Structure']
        require(not p['semiPersistent'] and not p['dontSave'] and not p['persistentBoolData']['activated'], 'Changed persistent object')
    require(read(3612)['m_Structure']['boolName']=='crossroadsInfected', 'Changed infected pillar visibility')
    # Target collision disappears at Open, before animation completion.
    disabled = [a for a in next(s for s in pillar['states'] if s['name']=='Open')['actions']
                if a['enabled'] and a['type'].endswith('.SetCollider')]
    require(len(disabled)==1 and not disabled[0]['parameters']['active']['value'], 'Changed pillar collision dispatch')
    for state,owners in (('Shake',[123]),('Open',[122,121]),('Finish Opening',[119,118,965])):
        rows=[a['parameters'] for s in pillar['states'] if s['name']==state for a in s['actions']
              if a['enabled'] and a['type'].endswith('.PlayParticleEmitter')]
        require([a['gameObject']['gameObject']['value']['m_PathID'] for a in rows]==owners and
                all(a['emit']['value']==0 for a in rows),'Changed pillar particle phase dispatch')
    for state,clip in (('Shake',dict(m_FileID=15,m_PathID=17)),('Finish Opening',dict(m_FileID=2,m_PathID=170))):
        sound=action(pillar,state,'AudioPlayerOneShotSingle')
        require(sound['audioClip']['value']==clip and sound['audioPlayer']['value']==dict(m_FileID=5,m_PathID=4126) and
                sound['spawnPoint']['name']=='Self' and
                all(not sound[k]['useVariable'] and sound[k]['value']==v for k,v in
                    (('pitchMin',1),('pitchMax',1),('volume',1),('delay',0))), 'Changed original pillar sound')
    parts=[]
    for owner,transform,sprite in ((512,1509,3215),(120,1122,3209)):
        go=read(owner)
        require(go['m_IsActive'], 'Inactive authored owner')
        chain=[read(transform)]
        while chain[-1]['m_Father']['m_PathID']:
            chain.append(read(chain[-1]['m_Father']['m_PathID']))
            require(len(chain)<32 and read(chain[-1]['m_GameObject']['m_PathID'])['m_IsActive'], 'Invalid source hierarchy')
        # Find the renderer by its serialized sorting fields, rather than assume an ID.
        renderers=[read(c['m_Component']['m_PathID']) for c in go['m_Components']]
        renderers=[r for r in renderers if 'm_SortingOrder' in r]
        require(len(renderers)==1 and renderers[0]['m_Enabled'], 'Missing original renderer')
        r=renderers[0]; s=read(sprite)['m_Structure']
        require(s['collection']==dict(m_FileID=3,m_PathID=180), 'Changed original collection')
        origin=project((0,0,0),chain)
        x=project((s['_scale']['x'],0,0),chain); y=project((0,s['_scale']['y'],0),chain)
        values=[x[0]-origin[0],y[0]-origin[0],origin[0],x[1]-origin[1],y[1]-origin[1],origin[1],origin[2],
                *[s['_color'][c] for c in 'rgba'],r['m_SortingLayer'],r['m_SortingOrder'],x[2]-origin[2],y[2]-origin[2],0]
        parts.append(dict(owner=owner,sprite=s['_spriteId'],values=values,chain=chain))
    c=read(2209)
    require(c['m_Enabled'] and c['m_IsTrigger'], 'Changed switch trigger')
    points=[project((c['m_Offset']['m_X']+x*c['m_Size']['m_X']/2,c['m_Offset']['m_Y']+y*c['m_Size']['m_Y']/2,0),parts[0]['chain'])
            for x in (-1,1) for y in (-1,1)]
    trigger=[min(p[0] for p in points),min(p[1] for p in points),max(p[0] for p in points),max(p[1] for p in points)]
    solid=read(2245)
    require(solid['m_Enabled'] and not solid['m_IsTrigger'], 'Changed pillar solid')
    return times,trigger,parts,hashes

def convert(output,source=SOURCE,art=ART,tex3ds=Path(TEX3DS_EXE)):
    require(is_private_output(output), 'Original artwork must remain private')
    times,trigger,parts,hashes=source_data(source)
    manifest=json.loads((art/'sources.json').read_text())
    for name,expected in manifest['sha256'].items():
        require(hashlib.sha256((art/name).read_bytes()).hexdigest()==expected, 'Changed art provenance: '+name)
    library=json.loads((art/manifest['parts']['library']['file']).read_text())['m_Structure']
    collection=json.loads((art/manifest['parts']['collection']['file']).read_text())['m_Structure']
    clips=[next(c for c in library['clips'] if c['name']==name) for name in NAMES]
    for clip in clips:
        require(clip['fps']>0 and clip['frames'], 'Invalid original clip')
        require(all(f['spriteCollection']==dict(m_FileID=0,m_PathID=180) and not f.get('triggerEvent') for f in clip['frames']), 'Unsupported frame event/reference')
    require(clips[1]['wrapMode']==clips[4]['wrapMode']==2, 'Source completion must remain Once')
    ids=sorted({f['spriteId'] for c in clips for f in c['frames']})
    texture=__import__('PIL.Image',fromlist=['Image']).open(art/manifest['textures'][0]).convert('RGBA')
    sprites=[(sid,*decode_sprite(texture,collection['spriteDefinitions'][sid])) for sid in ids]
    feedback,fx_sprites,fx_clips,emitters=compile_feedback(len(ids))
    # Bank sprite IDs are local array indices; original clip IDs stay mapped first.
    fx_ids=list(range(max(ids)+1,max(ids)+1+len(fx_sprites)))
    remap=dict(zip(range(len(ids),len(ids)+len(fx_sprites)),fx_ids))
    sprites += [(remap[sid],bitmap,rect) for sid,bitmap,rect in fx_sprites]
    for clip in fx_clips:
        for frame in clip['frames']:frame['spriteId']=remap[frame['spriteId']]
    ids += fx_ids;clips += fx_clips
    records,pages=pack_pages(sprites,[(1024,1024)])
    require(len(pages)<=2,'Pillar feedback exceeds two-page budget')
    output.mkdir(parents=True,exist_ok=True)
    for i,page in enumerate(pages):
        png=output/f'shaman-pillar-page-{i}.png';page.save(png)
        subprocess.run([str(tex3ds),'-f','rgba4','-z','none','-o',str(png.with_suffix('.t3x')),str(png)],check=True)
    (output/'shaman-pillar.bin').write_bytes(bank_bytes(clips,ids,records,len(pages)))
    raw=struct.pack('<4s7f3I',b'SHP1',*times,*trigger,512,120,2245)
    for p in parts:
        raw+=struct.pack('<II16f',p['owner'],ids.index(p['sprite']),*p['values'])
    (output/'shaman-pillar-room.bin').write_bytes(raw)
    (output/'shaman-pillar-fx.bin').write_bytes(feedback)
    write_audio(output)
    report=dict(scene='Crossroads_06',times=times,trigger=trigger,pages=len(pages),sprites=len(ids),source_sha256=hashes,
                art_sha256=manifest['sha256'],emitters=emitters,particle_capacity=80,pcm_bytes=755896,feedback_remaining='Switch hit prefab and explicit camera shake; sampled particle curves and shared ambient lighting approximation')
    (output/'shaman-pillar-manifest.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:report[k] for k in ('scene','times','trigger','pages','sprites')}))

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path,required=True)
    args=p.parse_args()
    convert(args.output)
