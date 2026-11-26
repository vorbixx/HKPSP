"""Pack original Last Stag/bell pixels, authored clips, English dialogue and PCM."""
import argparse, hashlib, json, math, shutil, struct, subprocess, wave, xml.etree.ElementTree as ET
import converted_files
from pathlib import Path
from PIL import Image
from assetripper_client import ROOT
from convert_hero import decode_sprite
from convert_cornifer import wrap_text
from atlas_packing import pack_pages
import tex3ds_cache
import text_fragment
from tool_paths import TEX3DS as TEX3DS_EXE

CLIPS=('Idle','Arrive','Arrive_smoke','Bell_stand','Bell_idle','Bell_ring L','Box_idle','Box_disappear')

def convert(source,output,romfs,tex3ds):
    if not output.resolve().is_relative_to((ROOT/'_private/converted').resolve()):raise ValueError('Output must be private converted')
    m=json.loads((source/'stag-map.json').read_text()); entries={};clips=[];atlases={}
    for name,digest in m['source_sha256'].items():
        if hashlib.sha256((source/name).read_bytes()).hexdigest()!=digest:raise ValueError('Changed source: '+name)
    if not set(CLIPS)<=set(c['name'] for c in m['clips']):raise ValueError('Missing Stag clip')
    for clip in m['clips']:
        if clip['name'] not in CLIPS:continue
        frames=[]
        for frame in clip['frames']:
            ref=frame['spriteCollection'];ck=f"{ref['m_FileID']}:{ref['m_PathID']}";sid=frame['spriteId'];key=(ck,sid)
            if key not in entries:
                coll=json.loads((source/m['collections'][ck]).read_text())['m_Structure'];sprite=coll['spriteDefinitions'][sid]
                tex=coll['textures'][sprite['materialId']];prefix=m['collections'][ck].rsplit('-',1)[0]
                name=f"{prefix}-{tex['m_PathID']}.png"
                if name not in atlases:atlases[name]=Image.open(source/name).convert('RGBA')
                bitmap,g=decode_sprite(atlases[name],dict(sprite,materialId=0))
                # Remove transparent margins without changing source pixels or world geometry.
                box=bitmap.getbbox()
                if box:
                    l,t,r,b=box;dx=(g[2]-g[0])/bitmap.width;dy=(g[3]-g[1])/bitmap.height
                    g=(g[0]+l*dx,g[3]-b*dy,g[0]+r*dx,g[3]-t*dy);bitmap=bitmap.crop(box)
                entries[key]=(bitmap,g)
            frames.append(key)
        clips.append(dict(name=clip['name'],fps=clip['fps'],wrap=clip['wrapMode'],loop=clip['loopStart'],frames=frames))
    # Station rooms share the existing native boss/room memory peak. No boss is resident here.
    # Runtime reserves room for at most fifteen resident Stag pages. Keep the
    # converter and the loader's admission limit in sync instead of emitting a
    # package that can never load on the 3DS.
    records,pages=pack_pages([(k,*v) for k,v in entries.items()],[(1024,1024)]*15)
    used=max(v[0] for v in records.values())+1;pages=pages[:used]
    output.mkdir(parents=True,exist_ok=True);romfs.mkdir(parents=True,exist_ok=True)
    ids=list(entries);index={k:i for i,k in enumerate(ids)};first=0
    with (output/'stag-anim.bin').open('wb') as f:
        f.write(struct.pack('<4s4I',b'HKA1',len(pages),len(ids),len(clips),sum(len(c['frames']) for c in clips)))
        for k in ids:f.write(struct.pack('<5I4f',*records[k]))
        for c in clips:
            f.write(struct.pack('<48sf4I',c['name'].encode(),c['fps'],c['wrap'],c['loop'],first,len(c['frames'])));first+=len(c['frames'])
        for c in clips:
            for k in c['frames']:f.write(struct.pack('<I',index[k]))
    names=['stag-anim.bin']
    for i,page in enumerate(pages):
        png=output/f'stag-page-{i}.png';t3x=output/f'stag-page-{i}.t3x';page.save(png)
        tex3ds_cache.run([str(tex3ds),'-f','rgba4','-z','none','-o',str(t3x),str(png)],check=True);names.append(t3x.name)
    text={e.attrib['name']:''.join(e.itertext()) for e in ET.parse(source/'EN_Stag-decrypted.xml').iter('entry')}
    lines=['#pragma once','namespace hk3ds::stagdata {','struct Dialogue {const char* const* pages;unsigned count;};']
    fragment={}  # Original pages go to the text pack; the header keeps "@Stag/<key>.<page>".
    for name,key in [('greeting','STAG_MEET'),('town','STAG_TOWN'),('crossroads','STAG_CROSSROADS'),('greenpath','STAG_GREENPATH'),('resting','STAG_RESTINGGROUNDS'),('fungal','STAG_FUNGUS'),('gardens','STAG_ROYALGARDENS'),('ruins1','STAG_RUINS1'),('ruins2','STAG_RUINS2'),('deepnest','STAG_DEEPNEST'),('nest','STAG_NEST'),('hidden','STAG_HIDDEN')]:
        pages_text=wrap_text(text[key]).split('\f')
        fragment.update({f'{key}.{i}':t for i,t in enumerate(pages_text)})
        lines.append('inline constexpr const char* '+name+'[] = {'+','.join(json.dumps(f'@Stag/{key}.{i}') for i in range(len(pages_text)))+'};')
        lines.append('inline constexpr unsigned '+name+'Count = '+str(len(pages_text))+';')
    lines+=['inline constexpr Dialogue local[]={{town,townCount},{crossroads,crossroadsCount},{greenpath,greenpathCount},{resting,restingCount},{fungal,fungalCount},{gardens,gardensCount},{ruins1,ruins1Count},{ruins2,ruins2Count},{deepnest,deepnestCount},{nest,nestCount},{hidden,hiddenCount}};','}']
    text_fragment.write(output,'Stag',fragment)
    (ROOT/'include/stag_dialogue_data.hpp').write_bytes(('\n'.join(lines)+'\n').encode('utf-8'))
    ffmpeg=ROOT/'_private/tools/ffmpeg-7.1.1/ffmpeg-7.1.1-full_build-shared/bin/ffmpeg.exe'
    if not ffmpeg.is_file():
        ffmpeg = shutil.which('ffmpeg') or ffmpeg
    audio=[]
    for dest,src in [('arrive','stag_arrive'),('voice','Stag_01'),('bell','bell_rise')]:
        wav=output/f'sfx-enemy-stag-{dest}.wav'
        subprocess.run([str(ffmpeg),'-v','error','-y','-i',str(source/(src+'.audio')),'-ac','1','-ar','16000','-acodec','pcm_s16le',str(wav)],check=True);names.append(wav.name)
        with wave.open(str(wav),'rb') as w:audio.append(dict(output=wav.name,pcm16_bytes=w.getnframes()*w.getnchannels()*2,streamed=False))
    (output/'stag-audio-manifest.json').write_text(json.dumps(dict(clips=audio,pcm_resident_budget_bytes=sum(c['pcm16_bytes'] for c in audio))))
    names.append('stag-audio-manifest.json')
    for name in names:shutil.copyfile(output/name,romfs/name)
    report=dict(pages=len(pages),texture_bytes=len(pages)*1024*1024*2,clips=[dict(name=c['name'],frames=len(c['frames'])) for c in clips],native_pixels=True)
    (output/'manifest.json').write_text(json.dumps(report,indent=2));print(json.dumps(report))

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',type=Path,default=ROOT/'_private/research/stag-stations');p.add_argument('--output',type=Path,default=ROOT/'_private/converted/stag');p.add_argument('--romfs',type=Path,default=ROOT/'_private/converted/stag/romfs');p.add_argument('--tex3ds',type=Path,default=Path(TEX3DS_EXE));a=p.parse_args();convert(a.source,a.output,a.romfs,a.tex3ds)
