"""Extract original Cornifer assets from the loaded copied Unity build."""
import argparse, hashlib, json
from pathlib import Path
from assetripper_client import Client, ROOT
from extract_crawler import Export, write_asset
from extract_tutorial_prompts import decrypt_language

def extract(client, output):
    output=output.resolve()
    shared_output=Path('C:/Projects/HK3DS/_private/research/cornifer').resolve()
    if not output.is_relative_to(ROOT/'_private/research') and output!=shared_output:
        raise ValueError('Cornifer source exports must stay in private research')
    output.mkdir(parents=True, exist_ok=True); e=Export(client,output); hashes={}
    def fetch(row):
        data,name=e.fetch(row['path']['C'],row['path']['D']); hashes[name]=e.hashes[name]
        return data,name,row['path']['C']
    def image(c,d,name):
        raw=client.request('/Assets/Image',Path=json.dumps({'C':c,'D':d},separators=(',',':')),Extension='png')
        write_asset(output/name,raw); hashes[name]=hashlib.sha256(raw).hexdigest()
    # Search the small scene-manager index for level64, then fetch known object
    # IDs directly. A broad "Cornifer" search can omit the GameObject from its
    # result page because the same name occurs on many room-map sprites.
    managers=[r for r in client.search('SceneManager')
              if r['type']=='GameObject' and r['collection']=='level64']
    if not managers: raise ValueError('Expected a SceneManager in level64 (build 22529139)')
    nc=managers[0]['path']['C']
    npc,npc_file=e.fetch(nc,634); npc_path={'C':nc,'D':634}
    if npc.get('m_Name')!='Cornifer':
        raise ValueError(f'level64 object 634 is {npc.get("m_Name")!r}, expected Cornifer')
    parts=e.components(nc,634)
    animators=[]
    for names in parts.values():
        for name in names:
            value=e.cache[name].get('m_Structure',{})
            if 'library' in value and 'defaultClipId' in value: animators.append(value)
    if len(animators)!=1: raise ValueError(f'Expected one tk2d animator; found {len(animators)}')
    animator=animators[0]; lib,lib_file,lc=e.resolve(nc,animator['library']); clips=lib['m_Structure']['clips']
    collections={}; textures={}
    for clip in clips:
        for frame in clip['frames']:
            ref=frame['spriteCollection']; key=f"{ref['m_FileID']}:{ref['m_PathID']}"
            if key in collections: continue
            coll,filename,sc=e.resolve(lc,ref); collections[key]=filename
            for tex in coll['m_Structure']['textures']:
                _,tf,tc=e.resolve(sc,tex); png=tf.removesuffix('.json')+'.png'
                if png not in textures: image(tc,tex['m_PathID'],png); textures[png]=tf
    card,card_file=e.fetch(nc,1132); card_path={'C':nc,'D':1132}
    if card.get('m_Name')!='Cornifer Card':
        raise ValueError(f'level64 object 1132 is {card.get("m_Name")!r}, expected Cornifer Card')
    cp=e.components(nc,1132); refs=[]
    for name in cp.get('SpriteRenderer',[]):
        ref=e.cache[name]['m_Sprite']
        if ref['m_PathID']:
            sprite,sf,sc=e.resolve(nc,ref); tex=sprite['m_RD']['m_Texture']; _,tf,tc=e.resolve(sc,tex)
            png=tf.removesuffix('.json')+'.png'
            if png not in textures: image(tc,tex['m_PathID'],png); textures[png]=tf
            refs.append(dict(sprite=sf,texture=png))
    card['sprites']=refs
    languages={}
    for key in ('EN_Cornifer','EN_UI'):
        langs=[r for r in client.search(key) if r['name']==key]
        if len(langs)!=1: raise ValueError(f'Expected one {key} asset; found {len(langs)}')
        language,filename,_=fetch(langs[0]); xml=decrypt_language(language['m_Script'])
        decrypted=f'{key}-decrypted.xml'
        (output/decrypted).write_bytes(xml.encode('utf-8'))
        hashes[decrypted]=hashlib.sha256(xml.encode()).hexdigest()
        languages[key]=filename
    audio={}
    for name in ('Cornifer_Hum','Cornifer_03'):
        found=[r for r in client.search(name) if r['type']=='AudioClip' and r['name']==name and r['collection']=='sharedassets64.assets']
        if len(found)==1:
            filename=name+'.audio'; raw=client.request('/Assets/Audio',Path=json.dumps(found[0]['path'],separators=(',',':')))
            write_asset(output/filename,raw); hashes[filename]=hashlib.sha256(raw).hexdigest(); audio[name]=filename
    card_textures=[r for r in client.search('cornifer_card') if r['type']=='Texture2D' and r['name']=='cornifer_card' and r['collection']=='sharedassets64.assets']
    if len(card_textures)==1: image(card_textures[0]['path']['C'],card_textures[0]['path']['D'],'cornifer-card.png')
    papers={}
    for query in ('paper','Cornifer paper','Map Paper'):
        for r in client.search(query): papers[(r['collection'],r['path']['D'],r['type'])]=r
    (output/'paper-candidates.json').write_text(json.dumps(list(papers.values()),indent=2),encoding='utf-8')
    manifest=dict(build_id=22529139,npc=npc_file,npc_path=npc_path,parts=parts,animator=animator,
      library=lib_file,clips=clips,collections=collections,textures=textures,card=card,languages=languages,
      audio=audio,paper_candidates=len(papers),source_sha256=hashes)
    (output/'cornifer-map.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
    print(json.dumps(dict(clips=[c['name'] for c in clips],collections=len(collections),card=bool(card),
      audio=sorted(audio),paper_candidates=len(papers))))

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__); p.add_argument('--port',type=int,default=58291)
    p.add_argument('--output',type=Path,default=ROOT/'_private/research/cornifer')
    a=p.parse_args(); extract(Client(a.port),a.output)
