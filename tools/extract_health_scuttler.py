"""Export the one native Health Scuttler pickup sprite from the copied build."""
import hashlib
import json
from assetripper_client import Client, ROOT
from extract_crawler import Export


class CachedExport(Export):
    def fetch(self, collection, asset_id):
        name, _ = self.collection(collection)
        key = f'{name}-{asset_id}.json'
        if (self.output/key).exists():
            raw = (self.output/key).read_bytes()
            self.cache[key] = json.loads(raw)
            self.hashes[key] = hashlib.sha256(raw).hexdigest()
            return self.cache[key], key
        return super().fetch(collection, asset_id)


def extract(client=None, output=None):
    output = output or (ROOT/'_private/research/geo-visuals').resolve()
    output.mkdir(parents=True, exist_ok=True)
    client = client or Client(58291)
    rows = [r for r in client.search('Spatter Blue Heal')
            if r['type'] == 'GameObject' and r['name'] == 'Spatter Blue Heal'
            and r['collection'] == 'sharedassets6.assets' and r['path']['D'] == 464]
    if len(rows) != 1 or rows[0]['path']['D'] != 464:
        raise ValueError('Expected copied Tutorial_01 build22529139 projectile464')
    e = CachedExport(client, output)
    collection = rows[0]['path']['C']
    mapping_path = output/'geo-visuals-map.json'
    mapping = json.loads(mapping_path.read_text())
    expected = json.loads((output/'sharedassets6.assets-1092.json').read_text())
    actual = json.loads(client.asset({'C':collection, 'D':1092}))
    if actual != expected or mapping['build_id'] != 22529139:
        raise ValueError('Loaded animation library differs from copied build22529139')
    parts = e.components(collection, 464)
    sprite, sprite_key, sc = e.resolve(collection, e.cache[parts['SpriteRenderer'][0]]['m_Sprite'])
    texture_ref = sprite['m_RD']['m_Texture']
    _, texture_key, tc = e.resolve(sc, texture_ref)
    png = texture_key.removesuffix('.json')+'.png'
    raw = client.request('/Assets/Image', Path=json.dumps({'C':tc, 'D':texture_ref['m_PathID']}), Extension='png')
    (output/png).write_bytes(raw)
    e.hashes[png] = hashlib.sha256(raw).hexdigest()
    group = dict(owner=464, kind='heal', name='Spatter Blue Heal', parts=parts,
                 native_frames=[dict(sprite=sprite_key, texture=png)])
    mapping['groups'] = [g for g in mapping['groups'] if g['kind'] != 'heal']+[group]
    mapping['source_sha256'].update(e.hashes)
    mapping_path.write_text(json.dumps(mapping, indent=2), encoding='utf-8')
    print(json.dumps(dict(sprite=sprite_key, texture=png, files=len(e.hashes))))


if __name__ == '__main__':
    extract()
