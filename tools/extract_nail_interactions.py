"""Snapshot the two small interaction prefabs from the copied build's AssetRipper."""
import hashlib
import json
import io
import urllib.error
from PIL import Image
from assetripper_client import Client, ROOT
from extract_crawler import Export


def extract(client=None, output=None, game_data=None):
    output = output or ROOT / '_private/research/nail-interactions'
    game_data = game_data or ROOT / '_private/original/Hollow Knight/hollow_knight_Data'
    output.mkdir(parents=True, exist_ok=True)
    client = client or Client()
    export = Export(client, output)
    mapping = dict(build=22529139, trees={}, animations={}, textures={}, sprites={})

    def image(collection, ref):
        texture, key, tc = export.resolve(collection, ref)
        png = key.removesuffix('.json')+'.png'
        try:
            raw = client.request('/Assets/Image', Path=json.dumps(dict(C=tc, D=ref['m_PathID'])), Extension='png')
        except urllib.error.HTTPError as error:
            if error.code != 404 or texture['m_Format'] != 1:
                raise
            stream = texture['m_StreamData']
            path = game_data / stream['m_Path']
            with path.open('rb') as source:
                source.seek(stream['m_Offset']);alpha = source.read(stream['m_Size'])
            mask = Image.frombytes('L',(texture['m_Width'],texture['m_Height']),alpha).transpose(Image.Transpose.FLIP_TOP_BOTTOM)
            bitmap = Image.new('RGBA',mask.size,'white');bitmap.putalpha(mask)
            buffer=io.BytesIO();bitmap.save(buffer,format='PNG');raw=buffer.getvalue()
        (output/png).write_bytes(raw)
        export.hashes[png] = hashlib.sha256(raw).hexdigest()
        return png

    def tree(collection, owner, path):
        parts = export.components(collection, owner)
        mapping['trees'][path] = parts
        transform = export.cache[parts['Transform'][0]]
        for ref in transform['m_Children']:
            child, _, cc = export.resolve(collection, ref)
            go, _, gc = export.resolve(cc, child['m_GameObject'])
            tree(gc, child['m_GameObject']['m_PathID'], path+'/'+go['m_Name'])
        for kind in ('tk2dSpriteAnimator', 'tk2dSprite'):
            for name in parts.get(kind, []):
                data = export.cache[name]['m_Structure']
                if kind == 'tk2dSpriteAnimator':
                    library, key, lc = export.resolve(collection, data['library'])
                    mapping['animations'][key] = {}
                    refs = [f['spriteCollection'] for clip in library['m_Structure']['clips'] for f in clip['frames']]
                else:
                    refs = [data['collection']]
                    lc = collection
                for ref in {r['m_PathID']: r for r in refs}.values():
                    bank, key, bc = export.resolve(lc, ref)
                    if key in mapping['textures']:
                        continue
                    for texture in bank['m_Structure']['textures']:
                        mapping['textures'][key] = image(bc, texture)
        for key in parts.get('SpriteRenderer', []):
            renderer=export.cache[key]
            sprite, sk, sc = export.resolve(collection, renderer['m_Sprite'])
            mapping['sprites'][sk] = image(sc, sprite['m_RD']['m_Texture'])
            for ref in renderer['m_Materials']:
                export.resolve(collection,ref)
        if path == 'Arrow Prompt/Labels/Inspect':
            text = export.cache[parts['TextMeshPro'][0]]['m_Structure']
            font, fk, fc = export.resolve(collection, text['m_fontAsset'])
            mapping['font'] = dict(data=fk, atlas=image(fc, font['m_Structure']['atlas']))

    for title, owner, asset in [('Screen Flash',460,'sharedassets6.assets'),('Block Hit v2',4957,'resources.assets'),('Arrow Prompt',6142,'resources.assets')]:
        rows = [r for r in client.search(title) if r['type']=='GameObject' and r['collection']==asset and r['path']['D']==owner]
        if len(rows) != 1:
            raise ValueError('Missing copied source '+title)
        tree(rows[0]['path']['C'], owner, title)
    collection = rows[0]['path']['C']
    audio, key, ac = export.resolve(collection, dict(m_FileID=0, m_PathID=1278))
    raw = client.request('/Assets/Audio', Path=json.dumps(dict(C=ac, D=1278)))
    (output/'sword_hit_reject.ogg').write_bytes(raw)
    export.hashes['sword_hit_reject.ogg'] = hashlib.sha256(raw).hexdigest()
    mapping['audio'] = key
    level = next(r['path']['C'] for r in client.search('Tut_tablet_top') if r['collection']=='level6' and r['type']=='GameObject')
    for owner in (335,1361,905,1579,3541,2550):
        parts = export.components(level, owner)
        transform = export.cache[parts['Transform'][0]]
        if owner not in (335,1361):
            for ref in transform['m_Children']:
                child, _, cc = export.resolve(level, ref)
                go, _, _ = export.resolve(cc, child['m_GameObject'])
                if go['m_Name']=='Prompt Marker':
                    mapping.setdefault('markers', {})[str(owner)] = f"level6-{ref['m_PathID']}.json"
        while transform['m_Father']['m_PathID']:
            transform, _, _ = export.resolve(level, transform['m_Father'])
    mapping['source_sha256'] = export.hashes
    (output/'interaction-map.json').write_text(json.dumps(mapping, indent=2)+'\n')
    print(json.dumps(dict(nodes=len(mapping['trees']), files=len(export.hashes))))


if __name__ == '__main__':
    extract()
