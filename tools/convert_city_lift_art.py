"""Original 20-fps Crossroads_49b door pair; no replacement artwork."""
import converted_files
import argparse
import hashlib
import json
import struct
import subprocess
from pathlib import Path
from PIL import Image
from assetripper_client import ROOT
from atlas_packing import pack_pages
from convert_hero import decode_sprite
from convert_world_switch_visuals import bank_bytes
from convert_world_key_doors import require
from private_output_paths import is_private_output
from unity_transform import project
from tool_paths import TEX3DS as TEX3DS_EXE

ART=ROOT/'_private/minimum-obstacles/city-lift-art'
SOURCE=ROOT/'_private/research/crossroads-scenes/Crossroads_49b/visuals'

def source_art(art):
    manifest=json.loads((art/'sources.json').read_text())
    require(manifest['scene']=='Crossroads_49b','Wrong City lift art scene')
    for name,digest in manifest['sha256'].items():require(hashlib.sha256((art/name).read_bytes()).hexdigest()==digest,'Changed original City art')
    def part(name):return json.loads((art/manifest['parts'][name]['file']).read_text())['m_Structure']
    collection,library=part('collection'),part('library')
    clips=[next(c for c in library['clips'] if c['name']==name) for name in
           ('Lift Door Open','Lift Door Close','Call Switch Idle','Call Switch Left','Call Switch Right')]
    ident=int(Path(manifest['parts']['collection']['file']).stem.rsplit('-',1)[1])
    require(all(c['fps']==20 and c['wrapMode']==2 and len(c['frames'])==7 for c in clips[:2]) and
            all(c['fps']==12 for c in clips[2:]) and all(not f.get('triggerEvent') and
                f['spriteCollection']==dict(m_FileID=0,m_PathID=ident) for c in clips for f in c['frames']),
            'Changed City door/lever clip')
    ids=sorted({f['spriteId'] for c in clips for f in c['frames']})
    require(len(ids)==19 and len(manifest['textures'])==1,'Changed City door/lever frame set')
    return manifest,collection,clips,ids

def room_data(read,ids,parts=None,root_id=478):
    draws=[]
    for index,(owner,sprite,renderer,fsm_id,transform) in enumerate(parts or ((202,806,541,853,519),(201,805,543,844,513),(217,807,542,847,453))):
        from inspect_fsm import decode_fsm
        from convert_city_toll import one
        fsm=decode_fsm(read(fsm_id)['m_Structure']['fsm'])
        if index<2:
            require(one(fsm,'Open','Tk2dPlayAnimation')['clipName']['value']=='Lift Door Open' and
                    one(fsm,'Close','Tk2dPlayAnimation')['clipName']['value']=='Lift Door Close','Changed City door dispatch')
        else:require(one(fsm,'Idle','Tk2dPlayAnimation')['clipName']['value']=='Call Switch Idle','Changed City call switch')
        t,root=read(transform),read(root_id);s=read(sprite)['m_Structure'];r=read(renderer)
        require(t['m_Father']['m_PathID']==root_id and read(owner)['m_IsActive'] and s['_spriteId']==(40 if index<2 else 4),
                'Changed City door pose')
        scale=s['_scale'];color=s['_color'];o=project((0,0,0),[t,root]);x=project((scale['x'],0,0),[t,root]);y=project((0,scale['y'],0),[t,root])
        values=[x[0]-o[0],y[0]-o[0],o[0],x[1]-o[1],y[1]-o[1],o[1],o[2],color['r'],color['g'],color['b'],color['a'],
                r['m_SortingLayer'],r['m_SortingOrder'],x[2]-o[2],y[2]-o[2],0]
        draws.append(struct.pack('<2I16f',0,owner,*values))
    return struct.pack('<4s2I',b'CLA2',ids.index(40),ids.index(4))+b''.join(draws)

def convert(art,source,output,tex3ds):
    require(is_private_output(output),'City door output must remain private')
    manifest,collection,clips,ids=source_art(art)
    atlas=Image.open(art/manifest['textures'][0]).convert('RGBA')
    sprites=[(sid,*decode_sprite(atlas,collection['spriteDefinitions'][sid])) for sid in ids]
    records,pages=pack_pages(sprites,[(1024,1024)])
    output.mkdir(parents=True,exist_ok=True)
    for i,page in enumerate(pages):
        png=output/f'city-lift-art-page-{i}.png';page.save(png)
        subprocess.run([str(tex3ds),'-f','rgba4','-z','none','-o',str(output/f'city-lift-art-page-{i}.t3x'),str(png)],check=True)
    (output/'city-lift-art.bin').write_bytes(bank_bytes(clips,ids,records,len(pages)))
    from convert_city_lift import PROFILES
    level,_,_,root_id,_=PROFILES[source.parent.name]
    def read(n):return json.loads((source/f'level{level}-{n}.json').read_text())
    mapping=json.loads((source/'room-visuals-map.json').read_text())
    parts=[]
    def ident(name):return int(Path(name).stem.rsplit('-',1)[1])
    for row in sorted(mapping['mesh_renderer_audit'],key=lambda r:('Door L','Door R','Lift Call Lever').index(r['name']) if r.get('name') in ('Door L','Door R','Lift Call Lever') else 3):
        if row.get('name') not in ('Door L','Door R','Lift Call Lever'):continue
        c=row['components'];parts.append((ident(row['go']),ident(c['tk2dSprite'][0]),ident(row['renderer']),
                                        ident(c['PlayMakerFSM'][0]),ident(c['Transform'][0])))
    require(len(parts)==3,'Changed City door/lever group')
    (output/'city-lift-art-room.bin').write_bytes(room_data(read,ids,parts,root_id))
    print('City doors:',len(ids),'original sprites;',len(pages),'RGBA4 page;20fps,7 frames,mirrored source pair')

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--art',type=Path,default=ART)
    parser.add_argument('--source',type=Path,default=SOURCE);parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--tex3ds',type=Path,default=Path(TEX3DS_EXE))
    args=parser.parse_args();convert(args.art,args.source,args.output,args.tex3ds)
