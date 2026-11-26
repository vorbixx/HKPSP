"""Export/pack original ability art and sound; normal regeneration uses saved data."""
import converted_files  # Install hardlink copy-on-write before asset writes.
import argparse
import copy
import hashlib
import io
import json
from pathlib import Path
import shutil
import struct
import tex3ds_cache
from PIL import Image
from assetripper_client import Client, ROOT
from private_output_paths import is_private_output, is_private_source
from extract_crawler import Export
from convert_hero import decode_sprite
from convert_room_visuals import decode
from tool_paths import TEX3DS as TEX3DS_EXE

ART_HEADER=struct.Struct('<4sIIII')
ART_SPRITE=struct.Struct('<5I4f')
ART_CLIP=struct.Struct('<48sfIIII')
ART_FRAMES=struct.Struct('<I')
SHADE_SOUL_CLIPS=(('Shade Soul','Ball Lv2'),('Shade Soul Blast','Blast Lv2'))


def extract(source, port=None, *, client=None, include_shade_soul=True):
    source.mkdir(parents=True, exist_ok=True)
    client=client or Client(port);e=Export(client,source)
    sprites={};clips=[];textures={};provenance={}

    def actor(name, scene):
        row=next(r for r in client.search(name) if r['name']==name and r['collection']==scene and r['type']=='GameObject')
        return row['path']['C'],e.components(row['path']['C'],row['path']['D'])

    def texture(collection, ref):
        _,key,tc=e.resolve(collection,ref)
        if key not in textures:
            raw=client.request('/Assets/Image',Path=json.dumps({'C':tc,'D':ref['m_PathID']}),Extension='png')
            textures[key]=Image.open(io.BytesIO(raw)).convert('RGBA')
            provenance[key]=hashlib.sha256(raw).hexdigest()
        return textures[key]

    def animated(name,scene,aliases):
        collection,parts=actor(name,scene)
        library,lk,lc=e.resolve(collection,e.cache[parts['tk2dSpriteAnimator'][0]]['m_Structure']['library'])
        available={c['name']:c for c in library['m_Structure']['clips']}
        for alias,original in aliases.items():
            clip=copy.deepcopy(available[original]);clip['name']=alias
            for frame in clip['frames']:
                bank,key,bc=e.resolve(lc,frame['spriteCollection']);key=(key,frame['spriteId'])
                frame['key']=key
                if key in sprites:continue
                bank=bank['m_Structure'];definition=bank['spriteDefinitions'][frame['spriteId']]
                material,_,mc=e.resolve(bc,bank['materials'][definition['materialId']])
                ref=material['m_SavedProperties']['m_TexEnvs']['_MainTex']['m_Texture']
                definition=dict(definition,materialId=0)
                sprites[key]=decode_sprite(texture(mc,ref),definition)
            clips.append(clip)
        provenance[lk]=e.hashes[lk]

    def static(name,scene,alias):
        collection,parts=actor(name,scene)
        sprite,key,sc=e.resolve(collection,e.cache[parts['SpriteRenderer'][0]]['m_Sprite'])
        key=(key,0)
        sprites[key]=decode(texture(sc,sprite['m_RD']['m_Texture']),sprite)
        clips.append(dict(name=alias,fps=1,wrapMode=1,loopStart=0,frames=[dict(key=key)]))

    animated('Shaman Meeting','level76',{'Shaman Idle':'Idle','Shaman Talk':'Talk'})
    animated('Fireball','resources.assets',{'Spirit':'Ball','Spirit Blast':'Blast','Spirit End':'Ball End'})
    if include_shade_soul:
        animated('Fireball2 Spiral','resources.assets',{'Shade Soul':'Ball Lv2','Shade Soul Blast':'Blast Lv2'})
    animated('Dash Burst','resources.assets',{'Dash Effect':'Dash Effect','Cast':'Fireball1 Cast'})
    static('Spell Fireball','resources.assets','Spirit Icon')
    static('Dash Cloak','resources.assets','Cloak Icon')
    static('Shiny Item','level132','Cloak Pickup')

    keys=sorted(sprites);records={};pages=[Image.new('RGBA',(512,512))];x=y=row=0
    for key in sorted(keys,key=lambda k:(-sprites[k][0].height,-sprites[k][0].width,k)):
        bitmap,geometry=sprites[key]
        bitmap=bitmap.resize((max(1,round(bitmap.width*.25)),max(1,round(bitmap.height*.25))),Image.Resampling.LANCZOS)
        w,h=bitmap.size
        if w+2>512 or h+2>512:raise ValueError('Ability sprite exceeds atlas')
        if x+w+2>512:x=0;y+=row;row=0
        if y+h+2>512:pages.append(Image.new('RGBA',(512,512)));x=y=row=0
        pages[-1].paste(bitmap,(x+1,y+1));records[key]=(len(pages)-1,x+1,y+1,w,h,*geometry)
        x+=w+2;row=max(row,h+2)
    if len(pages)>1:raise ValueError('Ability art exceeds 512 KiB New 3DS budget')
    remap={k:i for i,k in enumerate(keys)}
    with (source/'ability-art.bin').open('wb') as out:
        out.write(struct.pack('<4sIIII',b'HKA1',len(pages),len(keys),len(clips),sum(len(c['frames']) for c in clips)))
        for key in keys:out.write(struct.pack('<5I4f',*records[key]))
        offset=0
        for clip in clips:
            out.write(struct.pack('<48sfIIII',clip['name'].encode(),clip['fps'],clip['wrapMode'],clip['loopStart'],offset,len(clip['frames'])))
            offset+=len(clip['frames'])
        for clip in clips:
            for frame in clip['frames']:out.write(struct.pack('<I',remap[frame['key']]))
    for i,page in enumerate(pages):page.save(source/f'ability-page-{i}.png')
    sound=next(r for r in client.search('hero_fireball') if r['type']=='AudioClip' and r['name']=='hero_fireball')
    raw=client.request('/Assets/Audio',Path=json.dumps(sound['path']))
    (source/'hero_fireball.ogg').write_bytes(raw)
    audio_source=ROOT/'_private/converted/enemy/source'
    audio_source.mkdir(parents=True,exist_ok=True)
    (audio_source/'resources-assets-1361-hero_fireball.ogg').write_bytes(raw)
    provenance['hero_fireball.ogg']=hashlib.sha256(raw).hexdigest()
    manifest=dict(build_id=22529139,pages=len(pages),sprites=len(keys),texture_bytes=len(pages)*524288,texture_format='rgba4',
                  clips=[c['name'] for c in clips],source_sha256={**e.hashes,**provenance},
                  approximations=['Quarter-resolution bitmap atlas; original sprite geometry and clip timing.',
                                  'Item panel and short Shaman dialogue reflowed for 400x240.'])
    (source/'manifest.json').write_text(json.dumps(manifest,indent=2))


def read_art_package(source):
    data=(source/'ability-art.bin').read_bytes()
    if len(data)<ART_HEADER.size: raise ValueError('Truncated ability-art.bin header')
    magic,pages,sprite_count,clip_count,frame_count=ART_HEADER.unpack_from(data)
    if magic!=b'HKA1' or not pages: raise ValueError('Invalid ability-art.bin header')
    sprite_offset=ART_HEADER.size;clip_offset=sprite_offset+ART_SPRITE.size*sprite_count
    frame_offset=clip_offset+ART_CLIP.size*clip_count
    if len(data)!=frame_offset+4*frame_count: raise ValueError('Invalid ability-art.bin length')
    page_images=[]
    for i in range(pages):
        page=Image.open(source/f'ability-page-{i}.png').convert('RGBA')
        if page.size!=(512,512): raise ValueError(f'Ability page {i} must be 512x512')
        page_images.append(page)
    sprites=[]
    for i in range(sprite_count):
        page,x,y,w,h,*geometry=ART_SPRITE.unpack_from(data,sprite_offset+i*ART_SPRITE.size)
        if page>=pages or not w or not h or x+w>512 or y+h>512: raise ValueError(f'Invalid ability sprite {i}')
        sprites.append({'bitmap':page_images[page].crop((x,y,x+w,y+h)),'geometry':geometry,'packed':True})
    indices=struct.unpack_from(f'<{frame_count}I',data,frame_offset) if frame_count else ()
    clips=[]
    for i in range(clip_count):
        name,fps,wrap,loop,offset,count=ART_CLIP.unpack_from(data,clip_offset+i*ART_CLIP.size)
        name=name.split(b'\0',1)[0].decode('utf-8')
        if offset+count>len(indices): raise ValueError(f'Invalid frame range for ability clip {name}')
        clips.append({'name':name,'fps':fps,'wrapMode':wrap,'loopStart':loop,'frames':list(indices[offset:offset+count])})
    if any(frame>=sprite_count for clip in clips for frame in clip['frames']): raise ValueError('Ability clip references missing sprite')
    return sprites,clips


def extract_shade_soul(client, output):
    row = next(r for r in client.search('Fireball2 Spiral')
               if r['collection'] == 'resources.assets' and r['type'] == 'GameObject'
               and r['name'] == 'Fireball2 Spiral' and r['path']['D'] == 4942)
    collection = row['path']['C']
    export = Export(client, output)
    parts = export.components(collection, 4942)
    animator = export.cache[parts['tk2dSpriteAnimator'][0]]
    library_ref = animator['m_Structure']['library']
    if library_ref['m_PathID'] != 23263:
        raise ValueError('Unexpected original Fireball2 Spiral library')
    library, _, library_collection = export.resolve(collection, library_ref)
    selected = {original for _, original in SHADE_SOUL_CLIPS}
    textures, frames, clips = {}, [], []
    for clip in library['m_Structure']['clips']:
        if clip['name'] not in selected:
            continue
        clips.append({**{key: clip[key] for key in ('name', 'fps', 'wrapMode', 'loopStart')},
                      'frame_count': len(clip['frames'])})
        for index, frame in enumerate(clip['frames']):
            bank, _, bank_collection = export.resolve(library_collection, frame['spriteCollection'])
            bank = bank['m_Structure']
            definition = bank['spriteDefinitions'][frame['spriteId']]
            material, _, material_collection = export.resolve(bank_collection,
                bank['materials'][definition['materialId']])
            ref = material['m_SavedProperties']['m_TexEnvs']['_MainTex']['m_Texture']
            _, texture, texture_collection = export.resolve(material_collection, ref)
            if texture not in textures:
                raw = client.request('/Assets/Image', Path=json.dumps(
                    {'C': texture_collection, 'D': ref['m_PathID']}), Extension='png')
                textures[texture] = Image.open(io.BytesIO(raw)).convert('RGBA')
            bitmap, geometry = decode_sprite(textures[texture], dict(definition, materialId=0))
            filename = f'{len(frames):02d}.png'
            bitmap.save(output / filename)
            frames.append(dict(clip=clip['name'], frame=index,
                sprite_bank_path_id=frame['spriteCollection']['m_PathID'],
                sprite_id=frame['spriteId'], png=filename, geometry=geometry))
    if {clip['name'] for clip in clips} != selected:
        raise ValueError('Missing original Shade Soul clips')
    summary = dict(game_object=dict(name='Fireball2 Spiral', path_id=4942),
                   library=dict(source_path_id=23263), all_clips=clips,
                   extracted_frames=frames, asset_sha256=export.hashes)
    for filename, payload in (('animator', animator), ('library', library), ('summary', summary)):
        (output / f'fireball2-spiral-{filename}.json').write_text(json.dumps(payload, indent=2), encoding='utf-8')


def load_shade_soul(shade_source):
    summary_path=shade_source/'fireball2-spiral-summary.json'
    required=(summary_path,shade_source/'fireball2-spiral-animator.json',shade_source/'fireball2-spiral-library.json')
    if not all(path.is_file() for path in required): raise ValueError(f'Incomplete Shade Soul export: {shade_source}')
    summary=json.loads(summary_path.read_text())
    if (summary.get('game_object',{}).get('name')!='Fireball2 Spiral' or
            summary['game_object'].get('path_id')!=4942 or summary.get('library',{}).get('source_path_id')!=23263):
        raise ValueError('Expected Fireball2 Spiral GameObject 4942 and animator library 23263')
    animator=json.loads(required[1].read_text())
    library=json.loads(required[2].read_text())
    library_ref=animator.get('m_Structure',{}).get('library',{})
    if library_ref.get('m_PathID')!=23263: raise ValueError('Fireball2 Spiral animator does not reference library 23263')
    originals={clip['name']:clip for clip in summary.get('all_clips',[])}
    raw_clips={clip['name']:clip for clip in library.get('m_Structure',{}).get('clips',[])}
    source_frames={(frame['clip'],frame['frame']):frame for frame in summary.get('extracted_frames',[])}
    sprites=[];clips=[];provenance={}
    for alias,original in SHADE_SOUL_CLIPS:
        if original not in originals: raise ValueError(f'Missing source clip {original}')
        clip=originals[original];frames=[]
        raw_clip=raw_clips.get(original)
        if not raw_clip or (clip['fps'],clip['wrapMode'],clip['loopStart'],clip['frame_count'])!=(
                raw_clip['fps'],raw_clip['wrapMode'],raw_clip['loopStart'],len(raw_clip['frames'])):
            raise ValueError(f'Summary does not match source clip {original}')
        for frame_index in range(clip['frame_count']):
            frame=source_frames.get((original,frame_index))
            if not frame or len(frame.get('geometry',[]))!=4: raise ValueError(f'Missing source frame {original}[{frame_index}]')
            raw_frame=raw_clip['frames'][frame_index]
            if (frame.get('sprite_id')!=raw_frame['spriteId'] or
                    frame.get('sprite_bank_path_id')!=raw_frame['spriteCollection']['m_PathID']):
                raise ValueError(f'Summary does not match source frame {original}[{frame_index}]')
            image_path=shade_source/frame['png']
            if not image_path.is_file(): raise ValueError(f'Missing source sprite {image_path}')
            bitmap=Image.open(image_path).convert('RGBA')
            frames.append(len(sprites));sprites.append({'bitmap':bitmap,'geometry':frame['geometry']})
            provenance[f'shade-soul/{image_path.name}']=hashlib.sha256(image_path.read_bytes()).hexdigest()
        clips.append({'name':alias,'fps':clip['fps'],'wrapMode':clip['wrapMode'],'loopStart':clip['loopStart'],'frames':frames})
    for path in required: provenance[f'shade-soul/{path.name}']=hashlib.sha256(path.read_bytes()).hexdigest()
    for filename,digest in summary.get('asset_sha256',{}).items(): provenance[f'shade-soul/assets/{filename}']=digest
    return sprites,clips,provenance


def write_art_package(sprites,clips,output,manifest):
    output.mkdir(parents=True,exist_ok=True);pages=[Image.new('RGBA',(512,512))];records={};x=y=row=0
    bitmaps=[]
    for sprite in sprites:
        bitmap=sprite['bitmap']
        # Atlas crops already have the intended resolution; only source PNGs shrink.
        if not sprite.get('packed',False):
            bitmap=bitmap.resize((max(1,round(bitmap.width*.25)),max(1,round(bitmap.height*.25))),Image.Resampling.LANCZOS)
        bitmaps.append(bitmap)
    order=sorted(range(len(sprites)),key=lambda k:(-bitmaps[k].height,-bitmaps[k].width,k))
    for key in order:
        bitmap=bitmaps[key]
        w,h=bitmap.size
        if w+2>512 or h+2>512: raise ValueError('Ability sprite exceeds atlas')
        if x+w+2>512: x=0;y+=row;row=0
        if y+h+2>512: pages.append(Image.new('RGBA',(512,512)));x=y=row=0
        pages[-1].paste(bitmap,(x+1,y+1));records[key]=(len(pages)-1,x+1,y+1,w,h,*sprites[key]['geometry'])
        x+=w+2;row=max(row,h+2)
    if len(pages)>1: raise ValueError('Ability art exceeds 512 KiB New 3DS budget')
    remap={key:key for key in range(len(sprites))};total_frames=sum(len(clip['frames']) for clip in clips)
    with (output/'ability-art.bin').open('wb') as out:
        out.write(ART_HEADER.pack(b'HKA1',len(pages),len(sprites),len(clips),total_frames))
        for key in range(len(sprites)): out.write(ART_SPRITE.pack(*records[key]))
        offset=0
        for clip in clips:
            out.write(ART_CLIP.pack(clip['name'].encode(),clip['fps'],clip['wrapMode'],clip['loopStart'],offset,len(clip['frames'])))
            offset+=len(clip['frames'])
        for clip in clips:
            for frame in clip['frames']: out.write(ART_FRAMES.pack(remap[frame]))
    for index,page in enumerate(pages): page.save(output/f'ability-page-{index}.png')
    manifest.update(pages=len(pages),sprites=len(sprites),texture_bytes=len(pages)*512*512*2,
                    texture_format='rgba4',clips=[clip['name'] for clip in clips])
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2))


def combine_shade_soul(base_source,shade_source,output):
    sprites,clips=read_art_package(base_source)
    extra_sprites,extra_clips,provenance=load_shade_soul(shade_source)
    offset=len(sprites);sprites.extend(extra_sprites)
    for clip in extra_clips: clip['frames']=[frame+offset for frame in clip['frames']]
    clips.extend(extra_clips);manifest=json.loads((base_source/'manifest.json').read_text())
    manifest['source_sha256']={**manifest.get('source_sha256',{}),**provenance}
    manifest['shade_soul_source']={'game_object_path_id':4942,'animator_library_path_id':23263,
                                  'clips':{alias:{'original':original} for alias,original in SHADE_SOUL_CLIPS}}
    write_art_package(sprites,clips,output,manifest)


def convert(source, output, romfs, shade_soul_source=None):
    output.mkdir(parents=True,exist_ok=True);romfs.mkdir(parents=True,exist_ok=True)
    package=source
    if shade_soul_source is not None and shade_soul_source.is_dir():
        combine_shade_soul(source,shade_soul_source,output);package=output
    manifest=json.loads((package/'manifest.json').read_text())
    shutil.copy2(package/'ability-art.bin',romfs/'ability-art.bin')
    tex3ds=Path(TEX3DS_EXE)
    for i in range(manifest['pages']):
        tex3ds_cache.run([str(tex3ds),'-f','rgba4','-z','lz11','-o',str(romfs/f'ability-page-{i}.t3x'),str(package/f'ability-page-{i}.png')],check=True,capture_output=True)
    if package!=output: shutil.copy2(package/'manifest.json',output/'manifest.json')
    print(json.dumps({k:manifest[k] for k in ('pages','sprites','texture_bytes')}))


def validate_paths(source, output, romfs, root=ROOT):
    if not is_private_source(source, root):raise ValueError('Ability source must stay private')
    for path in (output,romfs):
        if not is_private_output(path, root):raise ValueError('Ability assets must stay private')


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--extract',action='store_true')
    p.add_argument('--port',type=int,default=58343)
    p.add_argument('--source',type=Path,default=ROOT/'_private/research/ability-pickups')
    p.add_argument('--shade-soul-source',type=Path,default=ROOT/'_private/minimum-abilities/spell-source/art')
    p.add_argument('--output',type=Path,default=ROOT/'_private/converted/ability-pickups')
    p.add_argument('--romfs',type=Path,default=ROOT/'_private/converted/hero/romfs')
    a=p.parse_args()
    if not any(a.source.resolve().is_relative_to(root.resolve()) for root in (ROOT/'_private',ROOT/'_private/research')):
        raise ValueError('Ability assets must stay private')
    for path in (a.output,a.romfs):
        if not converted_files.private_output(path, ROOT):raise ValueError('Ability assets must stay private')
    if a.extract:extract(a.source,a.port)
    convert(a.source,a.output,a.romfs,a.shade_soul_source)
