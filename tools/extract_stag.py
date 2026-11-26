"""Export Last Stag's original tk2d clips, language and sounds from a copied build.
Run under validation-lock.ps1 -Exclusive; starts a task-owned isolated AssetRipper.
"""
import argparse, hashlib, json, shutil, subprocess, time, urllib.request, urllib.parse
from pathlib import Path
from assetripper_client import Client, ROOT
from extract_crawler import Export, write_asset
from extract_tutorial_prompts import decrypt_language
from capture_original_frames import scene_assets


def extract_loaded(client, output):
    output.mkdir(parents=True, exist_ok=True)
    e=Export(client,output)
    rows=[r for r in client.search('SceneManager') if r['collection']=='level9' and r['type']=='GameObject']
    if len(rows)!=1:raise ValueError('Expected copied Room_Town_Stag_Station level9')
    c=rows[0]['path']['C'];animator,animator_file=e.fetch(c,1207)
    lib,lib_file,lc=e.resolve(c,animator['m_Structure']['library']);clips=list(lib['m_Structure']['clips'])
    bell,_,_=e.resolve(c,e.fetch(c,1201)[0]['m_Structure']['library'])
    clips += bell['m_Structure']['clips']
    collections={};textures={}
    for clip in clips:
        for frame in clip['frames']:
            ref=frame['spriteCollection'];key=f"{ref['m_FileID']}:{ref['m_PathID']}"
            if key in collections:continue
            coll,filename,sc=e.resolve(lc,ref);collections[key]=filename
            for tex in coll['m_Structure']['textures']:
                _,tf,tc=e.resolve(sc,tex);png=tf.removesuffix('.json')+'.png'
                if png not in textures:
                    write_asset(output/png,client.request('/Assets/Image',Path=json.dumps({'C':tc,'D':tex['m_PathID']}),Extension='png'))
                    textures[png]=tf
    languages={}
    for query in ('EN_Stag','EN_Prices','EN_UI'):
        rows=[r for r in client.search(query) if r['name'].lower()==query.lower()]
        if len(rows)!=1:raise ValueError(f'Expected one {query}, got {len(rows)}')
        d,n=e.fetch(rows[0]['path']['C'],rows[0]['path']['D']);xml=decrypt_language(d['m_Script'])
        filename=query+'-decrypted.xml';write_asset(output/filename,xml.encode());languages[query]=filename
    audio={}
    candidates=[r for r in client.search('stag') if r['type']=='AudioClip']
    for pid in (52,54):
        d,_,ac=e.resolve(c,{'m_FileID':3,'m_PathID':pid})
        candidates.append(dict(name=d['m_Name'],path={'C':ac,'D':pid}))
    for r in candidates:
        if r['name'] in audio:continue
        filename=r['name']+'.audio';write_asset(output/filename,client.request('/Assets/Audio',Path=json.dumps(r['path'])));audio[r['name']]=filename
    hashes={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in output.iterdir() if p.suffix in ('.json','.png','.xml','.audio') and p.name!='stag-map.json'}
    manifest=dict(build_id=22529139,animator=animator_file,library=lib_file,clips=clips,collections=collections,textures=textures,languages=languages,audio=audio,source_sha256=hashes)
    write_asset(output/'stag-map.json',json.dumps(manifest,indent=2).encode())
    print(json.dumps(dict(clips=[(c['name'],len(c['frames']),c['fps']) for c in clips],audio=sorted(audio),textures=list(textures))))


def extract(port=58298):
    output=ROOT/'_private/research/stag-stations'
    output.mkdir(parents=True,exist_ok=True)
    original=ROOT/'_private/original/Hollow Knight/hollow_knight_Data'
    source=ROOT/'_private/stag-extraction-input/hollow_knight_Data'
    source.mkdir(parents=True,exist_ok=True)
    names=set(scene_assets(original,9))|{'globalgamemanagers','globalgamemanagers.assets','resources.assets','resources.assets.resS'}
    for name in sorted(names):
        src=original/name
        if src.is_file():shutil.copy2(src,source/name)
    shutil.copytree(original/'Managed',source/'Managed',dirs_exist_ok=True)
    executable=ROOT/'_private/tools/AssetRipper-2.0.0/AssetRipper.GUI.Free.exe'
    log=(output/'assetripper.log').open('w')
    process=subprocess.Popen([str(executable),'--headless','--port',str(port)],cwd=executable.parent,
        stdout=log,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
    try:
        for attempt in range(90):
            try:
                urllib.request.urlopen(f'http://127.0.0.1:{port}/',timeout=2).close();break
            except OSError:time.sleep(1)
        else:raise RuntimeError('Task AssetRipper did not start')
        body=urllib.parse.urlencode({'path':str(source.parent)}).encode()
        urllib.request.urlopen(urllib.request.Request(f'http://127.0.0.1:{port}/LoadFolder',data=body),timeout=600).close()
        extract_loaded(Client(port), output)
    finally:
        process.terminate();process.wait(timeout=30);log.close()

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--port',type=int,default=58298)
    extract(parser.parse_args().port)
