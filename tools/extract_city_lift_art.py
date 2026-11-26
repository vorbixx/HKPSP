"""Recover only Crossroads_49b original tk2d lift-door clips and their atlas."""
import argparse
import hashlib
import json
from pathlib import Path
from assetripper_client import ROOT
from extract_crawler import Export
from extract_world_switch_session import run

def extract(client,source,output):
    rows=[r for r in client.search('elev_main') if r['type']=='GameObject' and r['name']=='elev_main' and r['collection']=='level80']
    if len(rows)!=1:raise ValueError('Load exactly one original Crossroads_49b')
    output.mkdir(parents=True,exist_ok=True);scene=rows[0]['path']['C'];exporter=Export(client,output)
    manifest=dict(scene='Crossroads_49b',parts={},textures=[],scene_sources_sha256={})
    for label,ident,field in [('collection',806,'collection'),('library',813,'library')]:
        path=source/f'level80-{ident}.json';raw=path.read_bytes();manifest['scene_sources_sha256'][path.name]=hashlib.sha256(raw).hexdigest()
        ref=json.loads(raw)['m_Structure'][field];data,key,target=exporter.resolve(scene,ref)
        manifest['parts'][label]=dict(file=key,collection=target)
        if label=='library':
            for clip in data['m_Structure']['clips']:
                if clip['name'] in ('Lift Door Open','Lift Door Close'):print(clip['name'],clip['fps'],len(clip['frames']),clip['wrapMode'])
        else:
            for texture_ref in data['m_Structure']['textures']:
                texture,key,c=exporter.resolve(target,texture_ref);png=key.removesuffix('.json')+'.png'
                raw=client.request('/Assets/Image',Path=json.dumps({'C':c,'D':texture_ref['m_PathID']}),Extension='png')
                (output/png).write_bytes(raw);exporter.hashes[png]=hashlib.sha256(raw).hexdigest();manifest['textures'].append(png)
                print('City door atlas',png,texture['m_Width'],texture['m_Height'])
    manifest['sha256']=exporter.hashes;(output/'sources.json').write_text(json.dumps(manifest,indent=2)+'\n')

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--port',type=int,default=58331)
    parser.add_argument('--output',type=Path,default=ROOT/'_private/minimum-obstacles/city-lift-art')
    args=parser.parse_args();run(args.port,args.output,scene='Crossroads_49b',level=80,extractor=extract)
