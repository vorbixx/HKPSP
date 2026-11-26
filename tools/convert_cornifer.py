"""Pack original Cornifer atlas clips, card, dialogue and optional PCM."""
import argparse, hashlib, html, json, math, re, shutil, struct, subprocess, xml.etree.ElementTree as ET
import converted_files  # Install hardlink copy-on-write before asset writes.
from pathlib import Path
from PIL import Image
from assetripper_client import ROOT
from convert_hero import decode_sprite
from atlas_packing import pack_pages
import tex3ds_cache
import text_fragment
from tool_paths import TEX3DS as TEX3DS_EXE

KEYS = ('CORNIFER_MEET','CROSSROADS_GREET','CORNIFER_AGAIN','CORNIFER_PROMPT','CORNIFER_REFUSE',
        'CORNIFER_NOT_ENOUGH','CROSSROADS_BOUGHT','CORNIFER_CARD','MAP_NAME_CROSSROADS')
# Cornifer sprites are Unity sprites at texelSize 1/64 (64 px per unit); downsample_sprite halves the
# atlas textures, so one atlas texel is 2/64 = 1/32 unit. Card extents must use the same scale.
ORIGINAL_PIXELS_PER_UNIT = 64
ATLAS_DOWNSAMPLE = 2
def card_extents(width, height):
    units_per_texel = ATLAS_DOWNSAMPLE / ORIGINAL_PIXELS_PER_UNIT
    half_w, half_h = width * units_per_texel / 2, height * units_per_texel / 2
    return (-half_w, -half_h, half_w, half_h)
ALIASES = {'CORNIFER_CARD':('CORNIFER_CARD','CARD','CORNIFER_MAP','MAP_CARD'),
           'MAP_NAME_CROSSROADS':('MAP_NAME_CROSSROADS','MAP_CROSSROADS')}

def downsample_sprite(bitmap, geometry):
    return bitmap.resize((max(1,bitmap.width//2),max(1,bitmap.height//2)),Image.Resampling.LANCZOS), geometry

def float_literal(value):
    text=f'{value:.8g}'
    if '.' not in text and 'e' not in text.lower(): text += '.0'
    return text+'f'

def wrap_text(value):
    value=html.unescape(value.replace('<br/>','\n').replace('<br>','\n'))
    for old,new in (( '\u2018',"'"),( '\u2019',"'"),( '\u201c','\"'),( '\u201d','\"'),( '\u2013','-'),( '\u2014','-'),( '\u2026','...'),( '\ufffd',"'")):
        value=value.replace(old,new)
    value=value.encode('ascii','replace').decode('ascii')
    pages=[]
    for page in re.split(r'<page\s*/?>|\f',value):
        lines=[]
        for para in page.splitlines() or ['']:
            words=para.split(); line=''
            for word in words:
                while len(word)>48:
                    part,word=word[:48],word[48:]
                    if line: lines.append(line); line=''
                    lines.append(part)
                if line and len(line)+1+len(word)>48: lines.append(line); line=word
                else: line=(line+' '+word).strip()
            if line or not words: lines.append(line)
        while len(lines)>4:
            pages.append('\n'.join(lines[:4])); lines=lines[4:]
        pages.append('\n'.join(lines))
    return '\f'.join(pages)

def dialogue(source):
    root=ET.parse(source/'EN_Cornifer-decrypted.xml')
    entries={e.attrib.get('name',''):''.join(e.itertext()) for e in root.iter('entry')}
    ui=ET.parse(source/'EN_UI-decrypted.xml')
    entries.update({e.attrib['name']:''.join(e.itertext()) for e in ui.iter('entry')
                    if e.attrib.get('name')=='MAP_NAME_CROSSROADS'})
    result=[]; actual={}
    for key in KEYS:
        candidates=ALIASES.get(key,(key,))
        found=next((k for k in candidates if k in entries),None)
        actual[key]=found
        result.append((key,wrap_text(entries[found]) if found else ''))
    return result,actual

def header(path, texts, actual, papers):
    def quote(s): return json.dumps(s,ensure_ascii=True)
    out=['#pragma once','#include <array>','#include <cstddef>','namespace hk3ds::corniferdata {',
      'inline constexpr float x=36.31f, y=34.69f, scaleX=1.1172f, scaleY=1.1451297f;',
      'struct Text { const char* key; const char* text; };',
      'inline constexpr Text texts[] = {']
    out += [f'  {{{quote(k)}, {quote(v)}}},' for k,v in texts]
    out += ['};','struct Paper { const char* scene; float x,y,scaleX,scaleY; const char* clip; };',
            f'inline constexpr std::array<Paper,{len(papers)}> papers{{{{']
    out += [f'  {{{quote(p["scene"])},{float_literal(p["x"])},{float_literal(p["y"])},{float_literal(p["scaleX"])},{float_literal(p["scaleY"])},{quote(p["clip"])} }},' for p in papers]
    out += ['}};','} // namespace hk3ds::corniferdata','']
    path.write_text('\n'.join(out),encoding='utf-8',newline='\n')
    return actual

def convert(source, output, romfs, tex3ds):
    source=source.resolve(); output=output.resolve(); romfs=romfs.resolve()
    if not source.is_relative_to((ROOT/'_private/research/cornifer').resolve()) or not output.is_relative_to((ROOT/'_private/converted').resolve()) or not romfs.is_relative_to((ROOT/'_private/converted').resolve()):
        raise ValueError('Cornifer source must stay in _private/research/cornifer; outputs must stay in _private/converted')
    m=json.loads((source/'cornifer-map.json').read_text())
    for name,digest in m['source_sha256'].items():
        if hashlib.sha256((source/name).read_bytes()).hexdigest()!=digest: raise ValueError('Changed source input '+name)
    clips=m['clips']; names={c['name']:c for c in clips}
    def find_clip(kind):
        options=[c for c in clips if c['name'].strip().lower()==kind]
        if not options: options=[c for c in clips if kind in c['name'].strip().lower()]
        if len(options)!=1: raise ValueError(f'Expected one original {kind} clip; candidates={[c["name"] for c in options]}')
        return options[0]
    selected=[('Idle',find_clip('idle')),('Talk L',find_clip('talk l')),('Talk R',find_clip('talk r'))]
    entries={}; ids=[]; sprite_records={}; clip_rows=[]
    collections={key:json.loads((source/name).read_text())['m_Structure'] for key,name in m['collections'].items()}
    atlases={name:Image.open(source/name).convert('RGBA') for name in m['textures']}
    for alias,clip in selected:
        frames=[]
        for frame in clip['frames']:
            ref=frame['spriteCollection']; coll_key=f"{ref['m_FileID']}:{ref['m_PathID']}"
            coll=collections[coll_key]; coll_file=Path(m['collections'][coll_key]).stem; tex_id=coll['textures'][0]['m_PathID']; texture_name=next(n for n,filename in m['textures'].items() if Path(filename).stem==f'{coll_file.rsplit("-",1)[0]}-{tex_id}')
            sid=frame['spriteId']; key=(coll_key,sid)
            if key not in entries:
                bitmap,geometry=decode_sprite(atlases[texture_name],coll['spriteDefinitions'][sid])
                if bitmap.width>510 or bitmap.height>510: raise ValueError('Cornifer source sprite exceeds atlas page')
                entries[key]=downsample_sprite(bitmap,geometry)
            frames.append(key)
        clip_rows.append(dict(name=alias,fps=float(clip['fps']),wrap=int(clip['wrapMode']),loop=int(clip['loopStart']),frames=frames))
    card_png=source/'cornifer-card.png'
    if not card_png.exists(): raise ValueError('Original cornifer_card texture is missing')
    card=Image.open(card_png).convert('RGBA')
    box=card.getbbox()
    if box: card=card.crop(box)
    card=card.resize((max(1,card.width//2),max(1,card.height//2)),Image.Resampling.LANCZOS)
    entries[('card',0)]=(card,card_extents(card.width,card.height))
    clip_rows.append(dict(name='Card',fps=1.,wrap=0,loop=0,frames=[('card',0)]))
    records,pages=pack_pages([(k,*entries[k]) for k in entries],[(512,512),(512,512)])
    ids=sorted(entries,key=lambda k:(str(k[0]),k[1])); first=0; total=sum(len(c['frames']) for c in clip_rows)
    output.mkdir(parents=True,exist_ok=True); romfs.mkdir(parents=True,exist_ok=True)
    bank=output/'cornifer-anim.bin'
    with bank.open('wb') as f:
        f.write(struct.pack('<4s4I',b'HKA1',len(pages),len(ids),len(clip_rows),total))
        for k in ids: f.write(struct.pack('<5I4f',*records[k]))
        for c in clip_rows:
            if len(c['name'].encode())>=48: raise ValueError('Clip name too long')
            f.write(struct.pack('<48sf4I',c['name'].encode(),c['fps'],c['wrap'],c['loop'],first,len(c['frames']))); first+=len(c['frames'])
        for c in clip_rows:
            for k in c['frames']: f.write(struct.pack('<I',ids.index(k)))
    for page_id,page in enumerate(pages):
        png=output/f'cornifer-page-{page_id}.png'; texture=output/f'cornifer-page-{page_id}.t3x'
        page.save(png)
        tex3ds_cache.run([str(tex3ds),'-f','rgba4','-z','none','-o',str(texture),str(png)],check=True)
    texts,actual=dialogue(source)
    papers=[] # Populate only from identified original room-prop transforms.
    # The original dialogue goes to the text pack; the header keeps keys only.
    keys=text_fragment.write(output,'Cornifer',dict(texts))
    header(ROOT/'include/cornifer_data.hpp',[(k,keys[k]) for k,_ in texts],actual,papers)
    names_out=['cornifer-anim.bin']+[f'cornifer-page-{i}.t3x' for i in range(len(pages))]
    for n in names_out: shutil.copyfile(output/n,romfs/n)
    audio_report=[]
    ffmpeg=ROOT/'_private/tools/ffmpeg-7.1.1/ffmpeg-7.1.1-full_build-shared/bin/ffmpeg.exe'
    if not ffmpeg.exists():
        ffmpeg = shutil.which('ffmpeg')
    if not ffmpeg:
        raise ValueError('Required FFmpeg decoder is missing')
    for stem,source_name in (('hum','Cornifer_Hum.audio'),('voice','Cornifer_03.audio')):
        audio_source=source/source_name
        if not audio_source.exists(): raise ValueError('Missing required original audio '+source_name)
        wav=output/f'cornifer-{stem}.wav'
        subprocess.run([str(ffmpeg),'-v','error','-y','-i',str(audio_source),'-ac','1','-acodec','pcm_s16le','-f','wav',str(wav)],check=True)
        shutil.copyfile(wav,romfs/wav.name)
        import wave
        with wave.open(str(wav),'rb') as stream: params=stream.getparams()
        audio_report.append(dict(file=wav.name,pcm_bytes=params.nframes*params.nchannels*2,sample_rate=params.framerate,channels=params.nchannels))
    report=dict(clips=[dict(name=c['name'],fps=c['fps'],frames=len(c['frames'])) for c in clip_rows],
      sprite_count=len(ids),pages=len(pages),texture_bytes=sum(p.width*p.height*2 for p in pages),dialogue_keys=actual,dialogue_pages={k:v.count('\f')+1 for k,v in texts},
      audio=audio_report,paper_props=0,limitations=['Card texture is downsampled 2:1; RGBA4 quantized.',
      'Paper props omitted until the original scene prop objects and transforms are resolved.'])
    (output/'manifest.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report))

def coll_key_file(mapping,key):
    return Path(mapping['collections'][key]).name

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source',type=Path,default=ROOT/'_private/research/cornifer')
    p.add_argument('--output',type=Path,default=ROOT/'_private/converted/cornifer-original')
    p.add_argument('--romfs',type=Path,default=ROOT/'_private/converted/cornifer-original/romfs')
    p.add_argument('--tex3ds',type=Path,default=Path(TEX3DS_EXE))
    a=p.parse_args(); convert(a.source,a.output,a.romfs,a.tex3ds)
