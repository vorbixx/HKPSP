"""Export only Tutorial_01 tablet and Glow Bug presentation dependencies."""
import hashlib
import json
from assetripper_client import Client, ROOT
from extract_crawler import Export


def extract(client=None, output=None):
    output = output or (ROOT/'_private/research/tablet-glow').resolve()
    output.mkdir(parents=True, exist_ok=True)
    client = client or Client()
    e = Export(client, output)
    rows = [r for r in client.search('Glow Bug')
            if r['type'] == 'GameObject' and r['collection'] == 'level6']
    if len(rows) != 11:
        raise ValueError(f'Expected 11 Glow Bugs, found {len(rows)}')
    rows = sorted(rows, key=lambda r:r['path']['D'])
    groups = []
    for row in rows:
        collection = row['path']['C']
        parts = e.components(collection, row['path']['D'])
        go = e.cache[parts['GameObject'][0]]
        for component in go['m_Components']:
            value, key, cc = e.resolve(collection, component['m_Component'])
            if 'm_Materials' in value:
                parts['MeshRenderer'] = [key]
                for ref in value['m_Materials']:
                    material, _, mc = e.resolve(cc, ref)
                    e.resolve(mc, material['m_Shader'])
        ancestors = []
        transform = e.cache[parts['Transform'][0]]
        root = row.get('root',row['path']['D'])
        for ref in transform['m_Children']:
            child, _, cc = e.resolve(collection, ref)
            child_go, _, gc = e.resolve(cc, child['m_GameObject'])
            if child_go['m_IsActive']:
                rows.append(dict(path={'C':gc,'D':child['m_GameObject']['m_PathID']},
                                 name=child_go['m_Name'],root=root))
        tc = collection
        while transform['m_Father']['m_PathID']:
            transform, name, tc = e.resolve(tc, transform['m_Father'])
            ancestors.append(name)
        animator = e.cache[parts['tk2dSpriteAnimator'][0]]['m_Structure']
        library, library_key, lc = e.resolve(collection, animator['library'])
        clip = library['m_Structure']['clips'][animator['defaultClipId']]
        collections = {}
        textures = {}
        for frame in clip['frames']:
            ref = frame['spriteCollection']
            refkey = f'{ref["m_FileID"]}:{ref["m_PathID"]}'
            if refkey in collections:
                continue
            coll, ck, cc = e.resolve(lc, ref)
            collections[refkey] = ck
            if len(coll['m_Structure']['textures']) != 1:
                raise ValueError('Expected a single Glow Bug texture')
            texture = coll['m_Structure']['textures'][0]
            _, tk, tc = e.resolve(cc, texture)
            png = tk.removesuffix('.json')+'.png'
            if png not in e.hashes:
                raw = client.request('/Assets/Image', Path=json.dumps({'C':tc,'D':texture['m_PathID']}), Extension='png')
                (output/png).write_bytes(raw)
                e.hashes[png] = hashlib.sha256(raw).hexdigest()
            textures[ck] = png
        groups.append(dict(owner=row['path']['D'], root=root, parts=parts, ancestors=ancestors,
                           library=library_key, clip=clip['name'],
                           sprite_collections=collections, textures=textures))
    collection = rows[0]['path']['C']
    tablets = []
    for owner in (905,1579,3541):
        parts = e.components(collection, owner)
        tablets.append(dict(owner=owner,parts=parts))
    particles = []
    for tablet,glows,rim in ((905,902,904),(1579,1577,1580),(3541,3539,3540)):
        for owner in (glows,rim):
            parts = e.components(collection,owner)
            transform = e.cache[parts['Transform'][0]]
            ancestors = []
            tc = collection
            while transform['m_Father']['m_PathID']:
                transform,name,tc = e.resolve(tc,transform['m_Father'])
                ancestors.append(name)
            renderer = e.cache[parts['ParticleSystemRenderer'][0]]
            material,mk,mc = e.resolve(collection,renderer['m_Materials'][0])
            _,sk,_ = e.resolve(mc,material['m_Shader'])
            texture = material['m_SavedProperties']['m_TexEnvs']['_MainTex']['m_Texture']
            _,tk,tc = e.resolve(mc,texture)
            png = tk.removesuffix('.json')+'.png'
            raw = client.request('/Assets/Image',Path=json.dumps({'C':tc,'D':texture['m_PathID']}),Extension='png')
            (output/png).write_bytes(raw)
            e.hashes[png] = hashlib.sha256(raw).hexdigest()
            particles.append(dict(owner=owner,tablet=tablet,kind='glows' if owner==glows else 'rim',
                                  parts=parts,ancestors=ancestors,material=mk,shader=sk,texture=png))
    mapping = dict(build_id=22529139, scene='Tutorial_01', groups=groups,
                   tablets=tablets, particles=particles, source_sha256=e.hashes)
    for name, digest in e.hashes.items():
        known = ROOT/'_private/research/room-visuals'/name
        if known.is_file() and hashlib.sha256(known.read_bytes()).hexdigest() != digest:
            raise ValueError('Loaded scene differs from the researched build: '+name)
    (output/'tablet-glow-map.json').write_text(json.dumps(mapping,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(dict(glow_bugs=len([g for g in groups if g['owner']==g['root']]),
                          animated_renderers=len(groups),tablets=tablets,files=len(e.hashes))))


if __name__ == '__main__':
    extract()
