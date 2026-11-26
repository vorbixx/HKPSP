"""Export only the Tutorial Buzzer animation library and its sprite dependencies."""
import argparse
import hashlib
import json
from pathlib import Path
from assetripper_client import Client, ROOT
from extract_crawler import Export


def resolve_library(client, output, source):
    rows=[r for r in client.search('Buzzer') if r['collection']=='level6' and r['name']=='Buzzer' and r['type']=='GameObject']
    if len(rows)!=1: raise ValueError('Load the private Tutorial_01 input first')
    inventory=json.loads((source/'inventory.json').read_text())
    row=next(r for r in inventory['rows'] if r['go']==f'level6-{rows[0]["path"]["D"]}.json')
    name=row['parts']['tk2dSpriteAnimator'][0]
    raw=(source/name).read_bytes()
    if hashlib.sha256(raw).hexdigest()!=inventory['source_sha256'][name]:
        raise ValueError('Changed animator input')
    e=Export(client,output)
    library,key,lc=e.resolve(rows[0]['path']['C'],json.loads(raw)['m_Structure']['library'])
    return e, library, key, lc


def extract(client, output):
    if not output.resolve().is_relative_to(ROOT/'_private'):
        raise ValueError('Original resources must remain private')
    output.mkdir(parents=True, exist_ok=True)
    e, library, key, lc = resolve_library(client, output, ROOT/'_private/research/tutorial')
    known=ROOT/'_private/research/room-entities/sharedassets6.assets-1150.json'
    if e.hashes[key]!=hashlib.sha256(known.read_bytes()).hexdigest():
        raise ValueError('Loaded library differs from researched build')
    collections={};textures={}
    for clip in library['m_Structure']['clips']:
        for frame in clip['frames']:
            ref=frame['spriteCollection']; refkey=f'{ref["m_FileID"]}:{ref["m_PathID"]}'
            if refkey in collections: continue
            collection,ck,sc=e.resolve(lc,ref);collections[refkey]=ck
            if len(collection['m_Structure']['textures'])!=1:
                raise ValueError('Expected one source texture')
            texture=collection['m_Structure']['textures'][0]
            _,tk,tc=e.resolve(sc,texture);png=tk.removesuffix('.json')+'.png'
            raw=client.request('/Assets/Image',Path=json.dumps({'C':tc,'D':texture['m_PathID']}),Extension='png')
            (output/png).write_bytes(raw);e.hashes[png]=hashlib.sha256(raw).hexdigest();textures[ck]=png
    mapping=dict(build_id=22529139,library=key,sprite_collections=collections,textures=textures,source_sha256=e.hashes)
    (output/'buzzer-map.json').write_text(json.dumps(mapping,indent=2))
    print(f'Buzzer: {len(collections)} collections, {len(textures)} textures')


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path,default=ROOT/'_private/research/buzzer')
    p.add_argument('--port',type=int,default=58291)
    a=p.parse_args();extract(Client(a.port),a.output)
