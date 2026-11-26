"""Export the Tutorial_01 rules records from the authored scene."""
import hashlib
import json

from extract_crawler import Export, write_asset


# GameObjects reviewed for Tutorial_01 in build 22529139. Their order defines
# existing bank indices. The complete collider inventory is exported separately.
RULE_OWNERS = (
    196, 254, 536, 783, 1615, 2017, 2221, 2229,
    2367, 2427, 2624, 2673, 2745, 2917, 2936, 3011,
    1543, 1707, 2050, 2237, 2368, 3048, 3525, 3631,
    282, 993, 1236, 1544, 1606, 1708, 2370, 2522,
    2835, 3526, 3630, 350, 352, 354, 767, 768,
    770, 877, 878, 880, 1315, 1316, 1317, 2567,
    2569, 2570, 2749, 2751, 2752, 2949, 2950, 2952,
    2992, 2993, 2994, 1215, 2707, 3086, 71, 198,
    335, 345, 715, 1125, 1361, 1493, 1519, 1529,
    1603, 1694, 1800, 1925, 1974, 1997, 2029, 2127,
    2227, 2268, 2298, 2449, 2495, 2854, 2861, 2863,
    3012, 3036, 3247, 3266, 3371, 3504, 3575, 3622,
    1296, 1339, 1388, 1638, 2208, 2362, 3359, 200,
    1490, 1900, 2670, 3078, 281, 990, 669, 2524,
    318,
)


def extract(client, output, terrain):
    manifest = json.loads((terrain / 'terrain-map.json').read_text(encoding='utf-8'))
    if manifest['build'] != 22529139 or manifest['scene'] != 'Tutorial_01':
        raise ValueError('Tutorial rules and terrain must come from the same supported scene')
    colliders = manifest['inventory']
    if len(colliders) != 351:
        raise ValueError('Tutorial_01 terrain must contain all 351 source colliders')
    crawler = [r for r in client.search('Crawler 1') if r['name'] == 'Crawler 1'
               and r['collection'] == 'level6' and r['type'] == 'GameObject']
    if len(crawler) != 1:
        raise ValueError('Load Tutorial_01 before exporting its rules')
    collection = crawler[0]['path']['C']
    export = Export(client, output)
    for name, expected in manifest['source_sha256'].items():
        if (terrain / name).parent != terrain:
            raise ValueError(f'Invalid Tutorial_01 terrain source name: {name}')
        raw = (terrain / name).read_bytes()
        if hashlib.sha256(raw).hexdigest() != expected:
            raise ValueError(f'Changed Tutorial_01 terrain record: {name}')
        write_asset(output / name, raw)
        export.hashes[name] = expected
        export.cache[name] = json.loads(raw)
    rules = []
    for identity in RULE_OWNERS:
        parts = export.components(collection, identity)
        rules.append(dict(name=export.cache[parts['GameObject'][0]]['m_Name'], parts=parts))
        transform = export.cache[parts['Transform'][0]]
        owner_collection = collection
        visited = {parts['Transform'][0]}
        while transform['m_Father']['m_PathID']:
            transform, key, owner_collection = export.resolve(owner_collection, transform['m_Father'])
            if key in visited:
                raise ValueError(f'Cyclic Tutorial_01 transform hierarchy for {identity}')
            visited.add(key)
    geometry_fields = ('type', 'collider', 'go', 'chain', 'owners')
    geometry = [{name: row[name] for name in geometry_fields} for row in colliders]
    inventory = [dict(type=row['type'], collider=row['collider'], go=row['go'],
                      parts=row['components'][0], chain=row['chain']) for row in colliders]
    for name, value in (('room-reliable-export.json', rules), ('collider-geometry.json', geometry),
                        ('inventory.json', dict(rows=inventory, source_sha256=export.hashes))):
        write_asset(output / name, (json.dumps(value, indent=2) + '\n').encode('utf-8'))
