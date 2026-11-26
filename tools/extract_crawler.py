"""Export Crawler 1 in Tutorial_01 and its immediate visual/physics dependencies.

Load the private analysis input with level6 and sharedassets6.* first. File IDs
are resolved through AssetRipper's collection dependency table, not session IDs.
Original data and the resulting provenance map remain private.
"""
import argparse
import hashlib
import html
import json
import os
import re
import tempfile
import urllib.parse
from pathlib import Path
from assetripper_client import Client, ROOT
from inspect_fsm import decode_fsm


def write_asset(path, raw):
    """Atomically replace one exported asset without mutating hardlink aliases."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix='.' + path.name + '.',
                                              suffix='.tmp', dir=path.parent)
    try:
        with os.fdopen(descriptor, 'wb') as output:
            output.write(raw)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def links(row, endpoint):
    for url,label in re.findall(r'<a[^>]+href="([^"]+)"[^>]*>(.*?)</a>',row):
        url=urllib.parse.urlsplit(html.unescape(url))
        if url.path==endpoint:
            yield json.loads(urllib.parse.parse_qs(url.query)['Path'][0]),html.unescape(re.sub('<[^>]+>','',label))


def native_component_kind(data):
    """Recognize serialized native components without ambiguous size fields."""
    if 'm_LocalPosition' in data:
        return 'Transform'
    if 'm_BodyType' in data:
        return 'Rigidbody2D'
    # SpriteRenderer also serializes m_Size. Never admit it as collision geometry.
    if 'm_Sprite' in data and 'm_Materials' in data:
        return 'SpriteRenderer'
    if 'm_InitialModule' in data and 'm_EmissionModule' in data:
        return 'ParticleSystem'
    if 'm_RenderMode' in data and 'm_MaxParticleSize' in data and 'm_Materials' in data:
        return 'ParticleSystemRenderer'
    if 'm_Size' in data and 'm_IsTrigger' in data and 'm_Offset' in data:
        return 'BoxCollider2D'
    if 'm_IsTrigger' in data and 'm_Offset' in data:
        if 'm_Radius' in data:
            return 'CircleCollider2D'
        if isinstance(data.get('m_Points'), dict) and 'm_Paths' in data['m_Points']:
            return 'PolygonCollider2D'
        if isinstance(data.get('m_Points'), list):
            return 'EdgeCollider2D'
    return None


class Export:
    def __init__(self, client, output):
        self.client,self.output=client,output
        self.collections={};self.cache={};self.hashes={}

    def collection(self, path):
        key=json.dumps(path,sort_keys=True)
        if key not in self.collections:
            page=self.client.request('/Collections/View',Path=json.dumps(path)).decode()
            # The self entry (index 0) is included in the dependency table.
            deps={}
            for row in re.findall(r'<tr>[\s\S]*?</tr>',page):
                cells=re.findall(r'<td>([\s\S]*?)</td>',row)
                if len(cells)!=2 or not cells[0].strip().isdigit(): continue
                resolved=list(links(cells[1],'/Collections/View'))
                if resolved: deps[int(cells[0])]=resolved[0]
            # Determine this collection's stable name from its page title.
            name=html.unescape(re.search(r'<title>(.*?)</title>',page)[1])
            deps[0]=(path,name)
            self.collections[key]=(name,deps)
        return self.collections[key]

    def fetch(self, collection, asset_id):
        name,_=self.collection(collection);key=f'{name}-{asset_id}.json'
        if key not in self.cache:
            raw=self.client.asset({'C':collection,'D':asset_id})
            self.cache[key]=json.loads(raw)
            write_asset(self.output/key, raw)
            self.hashes[key]=hashlib.sha256(raw).hexdigest()
        return self.cache[key],key

    def resolve(self, collection, ref):
        if not ref['m_PathID']: raise ValueError('Missing required reference')
        _,deps=self.collection(collection)
        target=deps[ref['m_FileID']][0]
        data,key=self.fetch(target,ref['m_PathID'])
        return data,key,target

    def components(self, collection, asset_id):
        go,key=self.fetch(collection,asset_id);result={'GameObject':[key]}
        for item in go['m_Components']:
            data,key,c=self.resolve(collection,item['m_Component'])
            if 'm_Script' in data:
                script,_,_=self.resolve(c,data['m_Script']);kind=script['m_ClassName']
            else: kind=native_component_kind(data)
            if kind is None: continue
            result.setdefault(kind,[]).append(key)
        return result


def extract(client, output):
    if not output.resolve().is_relative_to(ROOT/'_private'): raise ValueError('Export must remain private')
    output.mkdir(parents=True,exist_ok=True)
    rows=[r for r in client.search('Crawler 1') if r['type']=='GameObject' and r['name']=='Crawler 1' and r['collection']=='level6']
    if len(rows)!=1: raise ValueError('Load Tutorial_01 (level6) and sharedassets6.* with Managed first')
    row=rows[0];e=Export(client,output);c=row['path']['C']
    hero_rows=[r for r in client.search('HeroBox') if r['type']=='GameObject' and r['name']=='HeroBox' and r['collection']=='resources.assets']
    if len(hero_rows)!=1: raise ValueError('Expected one HeroBox')
    hr=hero_rows[0];hero_box=e.components(hr['path']['C'],hr['path']['D'])
    _,hero_parent,_=e.resolve(hr['path']['C'],e.cache[hero_box['Transform'][0]]['m_Father'])
    actor=e.components(c,row['path']['D'])
    transform=e.cache[actor['Transform'][0]];ancestors=[]
    while transform['m_Father']['m_PathID']:
        transform,key,_=e.resolve(c,transform['m_Father']);ancestors.append(key)
    death=e.cache[actor['EnemyDeathEffects'][0]]['m_Structure']
    corpse,corpse_key,cc=e.resolve(c,death['corpsePrefab'])
    corpse_parts=e.components(cc,death['corpsePrefab']['m_PathID'])
    materials=[]
    for group,collection in [(actor,c),(corpse_parts,cc)]:
        for name in group['BoxCollider2D']:
            ref=e.cache[name]['m_Material']
            if ref['m_PathID']:
                _,key,_=e.resolve(collection,ref);materials.append(key)
    anim=e.cache[actor['tk2dSpriteAnimator'][0]]['m_Structure']
    library,library_key,lc=e.resolve(c,anim['library'])
    sprite_collections={};textures={}
    for clip in library['m_Structure']['clips']:
        for frame in clip['frames']:
            ref=frame['spriteCollection'];rkey=f'{ref["m_FileID"]}:{ref["m_PathID"]}'
            if rkey in sprite_collections: continue
            coll,key,sc=e.resolve(lc,ref);sprite_collections[rkey]=key
            for texture in coll['m_Structure']['textures']:
                tex,texkey,tc=e.resolve(sc,texture)
                target=texkey.removesuffix('.json')+'.png'
                raw=client.request('/Assets/Image',Path=json.dumps({'C':tc,'D':texture['m_PathID']}),Extension='png')
                write_asset(output/target, raw);e.hashes[target]=hashlib.sha256(raw).hexdigest();textures[key]=target
    fsm=e.cache[actor['PlayMakerFSM'][0]]['m_Structure']['fsm']
    (output/'crawler-fsm.json').write_text(json.dumps(decode_fsm(fsm),indent=2),encoding='utf-8')
    manifest=dict(build=22529139,scene='Tutorial_01',actor=actor,ancestors=ancestors,corpse=corpse_parts,
                  library=library_key,sprite_collections=sprite_collections,textures=textures,materials=materials,
                  hero_box=hero_box,hero_parent=hero_parent,source_sha256=e.hashes)
    (output/'crawler-map.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
    print(json.dumps(dict(actor=actor,corpse=corpse_parts,files=len(e.hashes)),indent=2))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port',type=int,default=58291)
    parser.add_argument('--output',type=Path,default=ROOT/'_private/research/enemy')
    args=parser.parse_args();extract(Client(args.port),args.output)
