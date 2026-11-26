"""Build the five researched live Buzzer clips as a private HKA1 bank."""
import converted_files  # Install hardlink copy-on-write before asset writes.
import argparse
import hashlib
import json
import shutil
import struct
import tex3ds_cache
from pathlib import Path
from private_output_paths import is_private_output
from PIL import Image
from assetripper_client import ROOT
from convert_hero import decode_sprite
from inspect_fsm import decode_fsm
from tool_paths import TEX3DS as TEX3DS_EXE

CLIPS={'Idle':(12,0,3,5),'Startle':(12,2,0,4),'Chase':(12,0,0,4),
       'TurnToIdle':(12,1,2,7),'TurnToFly':(12,1,2,6)}


def observe(source, tutorial):
    mapping=json.loads((source/'buzzer-map.json').read_text())
    hashes={}
    def checked(root,name,expected):
        raw=(root/name).read_bytes();sha=hashlib.sha256(raw).hexdigest()
        if expected.get(name)!=sha: raise ValueError('Changed source: '+name)
        hashes[str((root/name).relative_to(ROOT))]=sha
        return json.loads(raw) if name.endswith('.json') else raw
    for name in mapping['source_sha256']: checked(source,name,mapping['source_sha256'])
    library=json.loads((source/mapping['library']).read_text())['m_Structure']
    clips=[c for c in library['clips'] if c['name'] in CLIPS]
    if {c['name'] for c in clips}!=set(CLIPS): raise ValueError('Missing live clips')
    for c in clips:
        if (c['fps'],c['wrapMode'],c['loopStart'],len(c['frames']))!=CLIPS[c['name']] or any(f['triggerEvent'] for f in c['frames']):
            raise ValueError('Changed clip contract: '+c['name'])
    inventory=json.loads((tutorial/'inventory.json').read_text());count=0
    def read(name):return checked(tutorial,name,inventory['source_sha256'])
    for row in inventory['rows']:
        if row['type']!='BoxCollider2D':continue
        # Resolve names from the inventory instead of assuming scene path IDs.
        if not read(row['go'])['m_Name'].startswith('Buzzer'):continue
        count+=1
        for key in row['chain']:
            t=read(key)
            if t['m_LocalScale']!={'m_X':1,'m_Y':1,'m_Z':1} or t['m_LocalRotation']!={'m_X':0,'m_Y':0,'m_Z':0,'m_W':1}:
                raise ValueError('Unsupported Buzzer transform')
        sprite=read(row['parts']['tk2dSprite'][0])['m_Structure']
        if sprite['_scale']!={'x':1,'y':1,'z':1} or sprite['_color']!={'r':1,'g':1,'b':1,'a':1}:
            raise ValueError('Unsupported sprite tint/scale')
        fsms=[read(k)['m_Structure']['fsm'] for k in row['parts']['PlayMakerFSM']]
        fsm=decode_fsm(next(f for f in fsms if f['name']=='chaser'))
        states={s['name']:s for s in fsm['states']}
        def action(state,kind):return next(a['parameters'] for a in states[state]['actions'] if a['enabled'] and a['type'].endswith('.'+kind))
        for state,clip in [('Idle','Idle'),('Startle','Startle'),('Stop','Idle'),('Chase Start','Chase')]:
            if action(state,'Tk2dPlayAnimation')['clipName']['value']!=clip:raise ValueError('Changed state clip')
        for state in ['Stop','Chase Start']:
            if action(state,'Tk2dPlayFrame')['frame']['value']!=3:raise ValueError('Changed starting frame')
        for state,clip in [('Idle','TurnToIdle'),('Chase - In Sight','TurnToFly'),('Chase - Out of Sight','TurnToFly')]:
            a=action(state,'FaceDirection')
            if a['spriteFacesRight']['value'] or not all(a[k] for k in ['playNewAnimation','everyFrame','pauseBetweenTurns']) or a['pauseTime']['value']!=.5 or a['newAnimationClip']['value']!=clip:
                raise ValueError('Changed turn contract')
        a=action('Startle','FaceObject')
        if a['spriteFacesRight']['value'] or a['playNewAnimation'] or a['resetFrame'] or a['everyFrame']:
            raise ValueError('Changed startle facing')
    if count!=3:raise ValueError('Expected three Tutorial Buzzers')
    return mapping,clips,hashes


def convert(source,tutorial,output,romfs,tex3ds):
    for path in [output]+([romfs] if romfs else []):
        if not converted_files.private_output(path, ROOT):raise ValueError('Resources must remain private')
        path.mkdir(parents=True,exist_ok=True)
    mapping,clips,hashes=observe(source,tutorial)
    collections={n:json.loads((source/n).read_text())['m_Structure'] for n in mapping['sprite_collections'].values()}
    if any(len(c['textures'])!=1 for c in collections.values()):raise ValueError('Unsupported texture mapping')
    atlases={n:Image.open(source/mapping['textures'][n]).convert('RGBA') for n in collections}
    def key(frame):
        r=frame['spriteCollection'];return mapping['sprite_collections'][f'{r["m_FileID"]}:{r["m_PathID"]}'],frame['spriteId']
    ids=sorted({key(f) for c in clips for f in c['frames']})
    sprites=[(k,*decode_sprite(atlases[k[0]],collections[k[0]]['spriteDefinitions'][k[1]])) for k in ids]
    sprites.sort(key=lambda s:(-s[1].height,-s[1].width,s[0]))
    page=Image.new('RGBA',(512,512));x=y=row=0;records={}
    for k,bitmap,geometry in sprites:
        w,h=bitmap.size
        if x+w+2>512:x=0;y+=row;row=0
        if w+2>512 or y+h+2>512:raise ValueError('Buzzer sprites exceed page capacity')
        page.paste(bitmap,(x+1,y+1));records[k]=(0,x+1,y+1,w,h,*geometry)
        x+=w+2;row=max(row,h+2)
    total=sum(len(c['frames']) for c in clips)
    with (output/'buzzer-anim.bin').open('wb') as out:
        out.write(struct.pack('<4s4I',b'HKA1',1,len(ids),len(clips),total))
        for k in ids:out.write(struct.pack('<5I4f',*records[k]))
        first=0
        for c in clips:
            out.write(struct.pack('<48sf4I',c['name'].encode(),c['fps'],c['wrapMode'],c['loopStart'],first,len(c['frames'])))
            first+=len(c['frames'])
        for c in clips:
            for f in c['frames']:out.write(struct.pack('<I',ids.index(key(f))))
    page.save(output/'buzzer-page-0.png')
    tex3ds_cache.run([str(tex3ds),'-f','rgba8','-z','none','-o',str(output/'buzzer-page-0.t3x'),str(output/'buzzer-page-0.png')],check=True)
    if romfs:
        for name in ['buzzer-anim.bin','buzzer-page-0.t3x']:shutil.copyfile(output/name,romfs/name)
    report=dict(build_id=22529139,source_sha256=hashes,clips=list(CLIPS),sprites=len(ids),frames=total,
                texture_bytes=512*512*4,limitations=['No corpse, hit flash, audio, particles or exact paced Unity animation-order comparison'])
    (output/'manifest.json').write_text(json.dumps(report,indent=2))
    print(json.dumps({k:v for k,v in report.items() if k!='source_sha256'}))


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source',type=Path,default=ROOT/'_private/research/buzzer')
    p.add_argument('--tutorial',type=Path,default=ROOT/'_private/research/tutorial')
    p.add_argument('--output',type=Path,default=ROOT/'_private/converted/buzzer')
    p.add_argument('--romfs',type=Path)
    p.add_argument('--tex3ds',type=Path,default=Path(TEX3DS_EXE))
    a=p.parse_args();convert(a.source,a.tutorial,a.output,a.romfs,a.tex3ds)
