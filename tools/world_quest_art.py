"""Export and compile the source Sly and Bretta rescue sprites and selected tk2d clips."""
import argparse
import copy
import hashlib
import json
import struct
import subprocess
from pathlib import Path

from PIL import Image
from assetripper_client import Client, ROOT
from extract_crawler import Export
from convert_hero import decode_sprite
from convert_area_title import title_data, raster_title
from world_quest_assets import quest_spec
from tool_paths import TEX3DS as TEX3DS_EXE

SOURCE = ROOT/'_private/minimum-world-quests/art'
OUTPUT = ROOT/'_private/converted/world-quest-art'
ROMFS = OUTPUT/'romfs'
TEX3DS = Path(TEX3DS_EXE)
CLIP_ALIASES = (('Sly Dazed','Dazed'),('Sly Wake','Wake'),('Sly Idle','Idle'))


def _private_path(path, root):
    resolved = path.resolve()
    if not resolved.is_relative_to(root.resolve()):
        raise ValueError(f'World quest artwork path escapes {root}')
    return resolved


def extract(client, output=SOURCE, quest='sly'):
    spec=quest_spec(quest)
    aliases=tuple((('Bretta' if quest=='bretta' else 'Sly')+' '+phase,phase) for phase in (('Dazed','Wake') if quest=='bretta' else ('Dazed','Wake','Idle')))
    output = _private_path(output, ROOT/'_private/minimum-world-quests')
    output.mkdir(parents=True, exist_ok=True)
    rows = [r for r in client.search(spec['name']) if r['type']=='GameObject' and
            r['name']==spec['name'] and r['collection']=='level'+str(spec['level']) and r['path']['D']==spec['owner']]
    if len(rows) != 1:
        raise ValueError('Expected reviewed source '+spec['name']+' owner')
    row=rows[0];e=Export(client, output);collection=row['path']['C'];parts=e.components(collection,spec['owner'])
    if len(parts.get('tk2dSpriteAnimator',[])) != 1 or len(parts.get('tk2dSprite',[])) != 1 or len(parts.get('Transform',[])) != 1:
        raise ValueError('Sly Dazed source animator/sprite/transform contract changed')
    animator=e.cache[parts['tk2dSpriteAnimator'][0]]['m_Structure']
    library,library_key,lc=e.resolve(collection,animator['library'])
    native_clips=library['m_Structure']['clips']
    names=[clip.get('name','') for clip in native_clips]
    by_name={clip['name']:clip for clip in native_clips if clip.get('name')}
    missing=[source_name for _,source_name in aliases if source_name not in by_name]
    if missing:
        raise ValueError(f'Required quest clips absent: {missing}; source clips: {names}')
    selected=[]
    for runtime_name,source_name in aliases:
        clip=copy.deepcopy(by_name[source_name]);clip['source_name']=source_name;clip['name']=runtime_name
        selected.append(clip)
    sprite_collections={};textures={};collection_textures={}
    for clip in selected:
        for frame in clip['frames']:
            ref=frame['spriteCollection'];refkey=f"{ref['m_FileID']}:{ref['m_PathID']}"
            bank,bank_key,bc=e.resolve(lc,ref)
            sprite_collections[refkey]=bank_key
            collection_textures.setdefault(bank_key,[])
            for texture_ref in bank['m_Structure']['textures']:
                _,texture_key,tc=e.resolve(bc,texture_ref)
                if texture_key not in collection_textures[bank_key]:
                    collection_textures[bank_key].append(texture_key)
                if texture_key not in textures:
                    raw=client.request('/Assets/Image',Path=json.dumps({'C':tc,'D':texture_ref['m_PathID']}),Extension='png')
                    png=texture_key.removesuffix('.json')+'.png'
                    (output/png).write_bytes(raw)
                    e.hashes[png]=hashlib.sha256(raw).hexdigest()
                    textures[texture_key]=png
    sprite_data=e.cache[parts['tk2dSprite'][0]]['m_Structure']
    transform=e.cache[parts['Transform'][0]]
    default_id=int(animator['defaultClipId'])
    default_source_name=names[default_id]
    default_runtime_name=next((runtime for runtime,source_name in aliases
                               if source_name==default_source_name),None)
    manifest=dict(build_id=22529139,scene=spec['scene'],owner=spec['name'],owner_id=spec['owner'],
        animator=animator,library=library_key,clips=selected,source_clip_names=names,
        clip_sources={runtime:source_name for runtime,source_name in aliases},
        default_clip_id=default_id,default_clip_name=default_runtime_name,
        default_clip_source_name=default_source_name,
        sprite=sprite_data,transform={k:transform[k] for k in ('m_LocalPosition','m_LocalRotation','m_LocalScale')},
        sprite_collections=sprite_collections,collection_textures=collection_textures,textures=textures,
        source_sha256=e.hashes)
    (output/'world-quest-art-source.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
    print(json.dumps(dict(clips=manifest['clip_sources'],default_clip=manifest['default_clip_name'],
                          sprite_collections=sprite_collections,textures=textures),indent=2),flush=True)


def compile_art(source=SOURCE, output=OUTPUT, romfs=ROMFS, tex3ds=TEX3DS, quest='sly'):
    source=_private_path(source,ROOT/'_private/minimum-world-quests')
    output=_private_path(output,ROOT/'_private/converted')
    romfs=_private_path(romfs,ROOT/'_private/converted')
    mapping=json.loads((source/'world-quest-art-source.json').read_text(encoding='utf-8'))
    clips=copy.deepcopy(mapping['clips']);collections={};atlases={}
    for clip in clips:
        for frame in clip['frames']:
            ref=frame['spriteCollection'];refkey=f"{ref['m_FileID']}:{ref['m_PathID']}"
            collection_key=mapping['sprite_collections'][refkey]
            if collection_key not in collections:
                data=json.loads((source/collection_key).read_text(encoding='utf-8'))['m_Structure']
                if len(data['textures']) != 1:
                    raise ValueError('Sly sprite collection uses multiple atlases')
                texture_keys=mapping['collection_textures'][collection_key]
                if len(texture_keys)!=1:
                    raise ValueError('Cannot map Sly collection to one original texture')
                texture_key=texture_keys[0]
                collections[collection_key]=data
                atlases[collection_key]=Image.open(source/mapping['textures'][texture_key]).convert('RGBA')
            frame['key']=(collection_key,frame['spriteId'])
    sprites={}
    for clip in clips:
        for frame in clip['frames']:
            key=frame['key']
            if key in sprites: continue
            collection=collections[key[0]]
            definition=collection['spriteDefinitions'][key[1]]
            if definition.get('materialId') != 0:
                raise ValueError('Sly clip uses an unsupported second atlas material')
            sprites[key]=decode_sprite(atlases[key[0]],definition)
    title_source=ROOT/'_private/research/area-title'
    title=None
    if quest=='sly':
        title=title_data(title_source, 'SLY')
        title_key=('npc-title',0)
        sprites[title_key]=raster_title(title_source,title)
        clips.append(dict(name='Sly Title',fps=1,wrapMode=1,loopStart=0,frames=[dict(key=title_key)]))
    prefix='bretta-quest' if quest=='bretta' else 'world-quest'
    pages=[Image.new('RGBA',(1024,1024))];records={};x=y=row=0
    for key in sorted(sprites,key=lambda k:(-sprites[k][0].height,-sprites[k][0].width,k)):
        bitmap,geometry=sprites[key];w,h=bitmap.size
        if w+2>1024 or h+2>1024: raise ValueError('Sly sprite exceeds 1024 atlas')
        if x+w+2>1024: x=0;y+=row;row=0
        if y+h+2>1024: pages.append(Image.new('RGBA',(1024,1024)));x=y=row=0
        pages[-1].paste(bitmap,(x+1,y+1));records[key]=(len(pages)-1,x+1,y+1,w,h,*geometry)
        x+=w+2;row=max(row,h+2)
    keys=sorted(sprites);remap={key:i for i,key in enumerate(keys)}
    output.mkdir(parents=True,exist_ok=True);romfs.mkdir(parents=True,exist_ok=True)
    binary=bytearray(struct.pack('<4sIIII',b'HKA1',len(pages),len(keys),len(clips),sum(len(c['frames']) for c in clips)))
    for key in keys: binary+=struct.pack('<5I4f',*records[key])
    offset=0
    for clip in clips:
        name=clip['name'].encode('utf-8')
        if len(name)>=48: raise ValueError('Sly source clip name exceeds HKA1 storage')
        binary+=struct.pack('<48sfIIII',name,clip['fps'],clip['wrapMode'],clip['loopStart'],offset,len(clip['frames']))
        offset+=len(clip['frames'])
    for clip in clips:
        for frame in clip['frames']: binary+=struct.pack('<I',remap[frame['key']])
    (romfs/(prefix+'-art.bin')).write_bytes(binary)
    for index,page in enumerate(pages):
        png=output/f'{prefix}-page-{index}.png';page.save(png)
        subprocess.run([str(tex3ds),'-f','rgba4','-z','lz11','-o',
                        str(romfs/f'{prefix}-page-{index}.t3x'),str(png)],check=True,capture_output=True)
    manifest={k:mapping[k] for k in ('build_id','scene','owner','owner_id','animator','sprite','transform',
                                     'clip_sources','default_clip_id','default_clip_name',
                                     'default_clip_source_name','source_sha256')}
    manifest.update(clips=[c['name'] for c in clips],sprites=len(keys),frames=offset,pages=len(pages),
                    npc_title=(dict(text=title['text'],x=title['x'],y=title['y'],
                                    source_sha256=title['mapping']['source_sha256']) if title else None),
                    texture_bytes=len(pages)*1024*1024*2,bitmap_scale=1,
                    sprite_geometry={str(k[1]):list(records[k][5:]) for k in keys})
    outputs=[romfs/(prefix+'-art.bin'),*(romfs/f'{prefix}-page-{i}.t3x' for i in range(len(pages)))]
    manifest['output_sha256']={path.name:hashlib.sha256(path.read_bytes()).hexdigest() for path in outputs}
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
    print(json.dumps({k:manifest[k] for k in ('clips','default_clip_name','sprites','frames','pages','texture_bytes')},indent=2))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--quest',choices=('sly','bretta'),default='sly')
    parser.add_argument('--extract',action='store_true',help='snapshot clips from an already loaded AssetRipper')
    parser.add_argument('--port',type=int,default=58373)
    args=parser.parse_args()
    source=SOURCE if args.quest=='sly' else SOURCE.parent/'bretta-art'
    output=OUTPUT if args.quest=='sly' else OUTPUT.parent/'bretta-quest-art'
    if args.extract: extract(Client(args.port),source,args.quest)
    else: compile_art(source,output,output/'romfs',quest=args.quest)


if __name__=='__main__': main()
