"""Export and decode tutorial action audio and the shared plant hit sound.

Requires AssetRipper loaded with resources.resource and a local soundfile
decoder (--decoder-dir). Steam inputs are read only; outputs stay private.
"""
import converted_files  # Install hardlink copy-on-write before asset writes.
import argparse,hashlib,json,sys,struct
from pathlib import Path
from assetripper_client import ROOT,Client
from extract_crawler import Export

def convert(decoder=None, *, client=None, out=None):
    if decoder:sys.path.insert(0,str(decoder.resolve()))
    import soundfile as sf
    client=client or Client();out=out or ROOT/'_private/research/tutorial-audio';out.mkdir(parents=True,exist_ok=True)
    e=Export(client,out)
    row=next(r for r in client.search('Knight') if r['collection']=='resources.assets' and r['type']=='GameObject' and r['name']=='Knight')
    collection=row['path']['C'];controller=e.fetch(collection,22332)[0]['m_Structure']
    audio={}
    for name in ('jump','softLanding','hardLanding','takeHit','footStepsRun'):
        source,key,sc=e.resolve(collection,controller[name]);audio[name]=(source,key,sc)
    combat=json.loads((ROOT/'_private/research/assets/combat-map.json').read_text())
    for item in combat:
        go,_=e.fetch(collection,item['game_object'])
        sources=[]
        for part in go['m_Components']:
            component,key,sc=e.resolve(collection,part['m_Component'])
            if 'm_Volume' in component and 'm_Resource' in component:sources.append((component,key,sc))
        if len(sources)!=1:raise ValueError('Expected one slash AudioSource')
        audio[item['kind']]=sources[0]
    entries={};romfs=ROOT/'_private/converted/hero/romfs'
    romfs.mkdir(parents=True,exist_ok=True)
    for name,(source,key,sc) in audio.items():
        clip,ck,cc=e.resolve(sc,source['m_Resource'])
        data=client.request('/Assets/Audio',Path=json.dumps({'C':cc,'D':source['m_Resource']['m_PathID']}))
        original=out/(name+'.ogg');original.write_bytes(data)
        samples,rate=sf.read(original,dtype='int16');target=romfs/('sfx-'+name+'.wav')
        sf.write(target,samples,rate,subtype='PCM_16')
        entries[name]=dict(source=key,clip=ck,volume=source['m_Volume'],pitch=source['m_Pitch'],
                           samples=len(samples),rate=rate,sha256=hashlib.sha256(target.read_bytes()).hexdigest())
    (out/'audio-map.json').write_text(json.dumps(dict(entries=entries,source_sha256=e.hashes),indent=2))
    (romfs/'sfx-settings.bin').write_bytes(struct.pack('<20f',*[v for entry in entries.values() for v in (entry['volume'],entry['pitch'])]))
    print(json.dumps(dict(clips=len(entries))))

def convert_plant(decoder=None, *, client=None, out=None):
    if decoder:sys.path.insert(0,str(decoder.resolve()))
    import soundfile as sf
    client=client or Client()
    matches=[row for row in client.search('rope_cut') if row['type']=='AudioClip' and row['collection']=='sharedassets6.assets']
    if len(matches)!=1:raise ValueError(f'Expected one sharedassets6 rope_cut clip, got {len(matches)}')
    clip=matches[0]
    out=out or ROOT/'_private/research/plant-audio';out.mkdir(parents=True,exist_ok=True)
    original=out/'rope_cut.ogg';original.write_bytes(client.request('/Assets/Audio',Path=json.dumps(clip['path'],separators=(',',':'))))
    samples,rate=sf.read(original,dtype='int16')
    target=ROOT/'_private/converted/hero/romfs/sfx-plant.wav'
    target.parent.mkdir(parents=True,exist_ok=True)
    sf.write(target,samples,rate,subtype='PCM_16')
    (out/'audio-map.json').write_text(json.dumps(dict(name='rope_cut',path=clip['path'],collection=clip['collection'],
        volume=1,pitch=[0.9,1.1],samples=len(samples),rate=rate,
        sha256=hashlib.sha256(target.read_bytes()).hexdigest()),indent=2))
    print(json.dumps(dict(plant_samples=len(samples),rate=rate)))

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--decoder-dir',type=Path);parser.add_argument('--plant-only',action='store_true')
    args=parser.parse_args()
    (convert_plant if args.plant_only else convert)(args.decoder_dir)
