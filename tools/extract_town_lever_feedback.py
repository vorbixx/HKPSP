"""Export the original Town lever sound, strike prefab and Hit Origin provenance."""
import hashlib
import json
from pathlib import Path
from extract_world_switch_session import run
from extract_crawler import Export
from inspect_fsm import decode_fsm
from convert_room_visuals import project

def extract(client, source, output):
    output.mkdir(parents=True,exist_ok=True)
    matches=[r for r in client.search('Mines Lever') if r['type']=='GameObject' and r['name']=='Mines Lever' and r['collection']=='level7']
    if len(matches)!=1:raise ValueError('Ambiguous Town lever')
    collection=matches[0]['path']['C'];export=Export(client,output)
    raw=(source.parent/'terrain/level7-3919.json').read_bytes()
    fsm=decode_fsm(json.loads(raw)['m_Structure']['fsm'])
    hit=next(s for s in fsm['states'] if s['name']=='Hit')
    actions={a['type'].split('.')[-1]:a['parameters'] for a in hit['actions'] if a['enabled']}
    report={'scene_fsm_sha256':hashlib.sha256(raw).hexdigest()}
    for name,kind,field in (('audio','AudioPlayerOneShotSingle','audioClip'),('effect','SpawnObjectFromGlobalPool','gameObject')):
        ref=actions[kind][field]['value'];data,key,linked=export.resolve(collection,ref)
        report[name]={'file':key,'name':data.get('m_Name'),'parameters':actions[kind]}
        if name=='effect':report[name]['components']=export.components(linked,ref['m_PathID'])
        else:
            audio=client.request('/Assets/Audio',Path=json.dumps({'C':linked,'D':ref['m_PathID']}))
            (output/'lever-hit.ogg').write_bytes(audio);export.hashes['lever-hit.ogg']=hashlib.sha256(audio).hexdigest()
    player,_,pc=export.resolve(collection,actions['AudioPlayerOneShotSingle']['audioPlayer']['value'])
    sources=[export.resolve(pc,c['m_Component'])[0] for c in player['m_Components']]
    audio_source=next(s for s in sources if 'm_AudioClip' in s)
    report['audio_source']=audio_source
    go,_=export.fetch(collection,316)
    transforms=[export.resolve(collection,c['m_Component'])[0] for c in go['m_Components']]
    transform=next(t for t in transforms if 'm_LocalPosition' in t)
    origins=[]
    for child in transform['m_Children']:
        t,_,tc=export.resolve(collection,child)
        owner,_,_=export.resolve(tc,t['m_GameObject'])
        if owner['m_Name']!='Hit Origin':continue
        chain=[t];cc=tc
        while chain[-1]['m_Father']['m_PathID']:
            parent,_,cc=export.resolve(cc,chain[-1]['m_Father']);chain.append(parent)
            if len(chain)>64:raise ValueError('Invalid Hit Origin hierarchy')
        origins.append(project((0,0,0),chain))
    if len(origins)!=1:raise ValueError('Missing or ambiguous Hit Origin')
    report['hit_origin']=origins[0]
    report['sha256']=export.hashes
    (output/'sources.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:{f:v for f,v in report[k].items() if f!='parameters'} for k in ('audio','effect')}))
    print('Authored Hit Origin:',report['hit_origin'])

if __name__ == '__main__':
    import argparse
    from assetripper_client import ROOT
    parser=argparse.ArgumentParser(description='Export original Town lever feedback and authored effect origin.')
    parser.add_argument('--port',type=int,default=58329)
    parser.add_argument('--output',type=Path,default=ROOT/'_private/minimum-obstacles/town-lever-feedback')
    args=parser.parse_args()
    run(args.port,args.output,scene='Town',level=7,extractor=extract)
