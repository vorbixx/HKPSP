"""Scene-local original Uumuu fight PCM, outside the resident enemy bank."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import shutil
import sys
import wave
import converted_files
from assetripper_client import ROOT

CLIPS=(('sharedassets34.assets',104,'zap_bug_pt1'),('sharedassets34.assets',96,'zap_bug_pt2'),
       ('sharedassets229.assets',9,'zap_bug'),
       ('sharedassets229.assets',11,'Quirrel_battle_intro'),
       ('sharedassets229.assets',7,'uumuu_helper_enter'),
       ('sharedassets229.assets',18,'uumuu_helper_exit'),
       ('sharedassets229.assets',8,'uumuu_helper_slash'),
       ('sharedassets229.assets',17,'uumuu_wounded'),
       ('sharedassets229.assets',16,'Quirrel_battle_shout_01'),
       ('sharedassets229.assets',15,'Quirrel_battle_shout_02'),
       ('sharedassets229.assets',14,'Quirrel_battle_dialogue_01'),
       ('sharedassets229.assets',10,'Quirrel_battle_dialogue_02'),
       ('sharedassets229.assets',13,'Quirrel_battle_dialogue_03'),
       ('sharedassets32.assets',135,'boss_final_hit'),
       ('sharedassets32.assets',62,'boss_gushing'),
       ('sharedassets32.assets',99,'boss_explode'))
CAP=9*1024*1024//4

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--extract',action='store_true')
    p.add_argument('--source',type=Path,default=ROOT/'_private/original/Hollow Knight/hollow_knight_Data')
    p.add_argument('--snapshot',type=Path,default=ROOT/'_private/minimum-bosses/uumuu-audio')
    p.add_argument('--output',type=Path,default=ROOT/'_private/converted/uumuu-audio')
    a=p.parse_args()
    if a.extract:
        sys.path[:0]=[str(ROOT/'_private/tools/boss-python'),str(ROOT/'_private/tools/audio-python')]
        import UnityPy
        env=UnityPy.load(*[str(a.source/n) for n in sorted({c[0] for c in CLIPS})])
        sources={f.name:f for f in env.files.values() if hasattr(f,'objects')}
        a.snapshot.mkdir(parents=True,exist_ok=True)
        for source_name,path_id,name in CLIPS:
            clip=sources[source_name].objects[path_id].read()
            if clip.m_Name!=name:raise ValueError(f'Unexpected source clip {path_id}: {clip.m_Name}')
            samples=clip.samples
            if len(samples)!=1:raise ValueError('Expected one original audio subsound')
            (a.snapshot/f'sfx-uumuu-{name}.wav').write_bytes(next(iter(samples.values())))
    entries=[]
    total=0
    for source_name,path_id,name in CLIPS:
        filename=f'sfx-uumuu-{name}.wav'
        data=(a.snapshot/filename).read_bytes()
        with wave.open(io.BytesIO(data),'rb') as f:
            if f.getsampwidth()!=2 or f.getnchannels() not in (1,2):raise ValueError('Expected original PCM16')
            size=f.getnframes()*f.getnchannels()*2
            entries.append(dict(source=f'{source_name}:{path_id}',name=name,output=filename,
                frames=f.getnframes(),rate=f.getframerate(),channels=f.getnchannels(),pcm_bytes=size,
                sha256=hashlib.sha256(data).hexdigest()))
        total+=size
    if total>CAP:raise ValueError(f'Uumuu scene audio exceeds {CAP}: {total}')
    a.output.mkdir(parents=True,exist_ok=True)
    for e in entries:shutil.copyfile(a.snapshot/e['output'],a.output/e['output'])
    (a.output/'manifest.json').write_text(json.dumps(dict(clips=entries,pcm_bytes=total,cap=CAP),indent=2)+'\n')
    print(f'Uumuu audio: {len(entries)} original PCM clips, {total} bytes')

if __name__=='__main__':main()
