"""Export only the two Great Door rigidbodies and their authored physics data."""
import json
from assetripper_client import Client, ROOT
from extract_crawler import Export


def extract(client=None, output=None):
    client = client or Client()
    row = next(r for r in client.search('Tute Great Door')
               if r['type'] == 'GameObject' and r['collection'] == 'sharedassets6.assets')
    output = output or ROOT / '_private/research/great-door-motion-source'
    output.mkdir(parents=True, exist_ok=True)
    exporter = Export(client, output)
    objects = {}
    for owner in (505, 482):
        parts = exporter.components(row['path']['C'], owner)
        game_object = exporter.cache[parts['GameObject'][0]]
        for component in game_object['m_Components']:
            raw, name, collection = exporter.resolve(row['path']['C'], component['m_Component'])
            if 'm_Points' in raw:
                parts['PolygonCollider2D'] = [name]
                _, material, _ = exporter.resolve(collection, raw['m_Material'])
                parts['PhysicsMaterial2D'] = [material]
        objects[str(owner)] = parts
    time = next(r for r in client.search('TimeManager') if r['type'] == 'TimeManager')
    _, time_file = exporter.fetch(time['path']['C'], time['path']['D'])
    manifest = dict(build_id=22529139, objects=objects, time=time_file, sha256=exporter.hashes)
    (output / 'motion-source-map.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('Exported two Great Door bodies, polygons, materials and TimeManager')


if __name__ == '__main__':
    extract()
