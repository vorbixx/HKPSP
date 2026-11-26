"""Bounded source adapter for the collapse and seven small-door emitters.

Only serialized presentation dependencies are exported; gameplay remains in the
existing room rules. Run --extract against the already loaded copied game data.
"""
import argparse
import copy
import hashlib
import json
import math
import struct
from pathlib import Path

from PIL import Image
from assetripper_client import Client, ROOT

SOURCE = ROOT / '_private/research/breakable-owner-effects-v2'
# Distinct presentation owners for the three once-only source floor bursts.
FLOOR_STAGES = {2422: 1, 2423: 2, 2421: 3}
COLLAPSE = (1026, 1525, 1817, 357, 491)
DOORS = {1296: (3826, 1299, 3862), 1339: (3898, 1340, 3802),
         1388: (3825, 1392, 3863), 1638: (3827, 1642, 3861),
         2208: (3899, 2206, 3801), 2362: (3824, 2358, 3865),
         3359: (3823, 3355, 3864)}


def crumble_floors(room, mapping):
    """Decode the instantaneous world-Z poses, including every rendered child.

    Dust Collapse Large is deliberately absent: Init looks for the misspelled
    'Dust Collapser Large'. FindChild stores null on a miss, so ActivateGameObject
    and PlayParticleEmitter never reach the scene child named 'Dust Collapse Large'.
    The checked supplemental export records that child and its playOnAwake=false.
    """
    from inspect_fsm import decode_fsm
    from convert_room_visuals import project
    source = OwnerSource()
    fsm = decode_fsm(source.read('level6-12043.json')['m_Structure']['fsm'])
    states = {s['name']: s for s in fsm['states']}
    names = {a['parameters']['storeResult']['name']: a['parameters']['childName']['value']
             for a in states['Idle']['actions'] if a['type'].endswith('.FindChild')}

    def read(key):
        raw = (room/key).read_bytes()
        if hashlib.sha256(raw).hexdigest() != mapping['source_sha256'][key]:
            raise ValueError('Changed Crumble room dependency: '+key)
        return json.loads(raw)

    root = source.objects['level6-318.json']['components']['Transform'][0]
    children = {}
    for pointer in source.read(root)['m_Children']:
        key = f"level6-{pointer['m_PathID']}.json"
        if key not in mapping['source_sha256']:
            continue
        transform = read(key)
        go = f"level6-{transform['m_GameObject']['m_PathID']}.json"
        children[read(go)['m_Name']] = (go, key)
    result = []
    for action in states['Crumble']['actions']:
        if not action['type'].endswith('.SetRotation'):
            continue
        p = action['parameters']
        if p['space'] != 0 or p['everyFrame'] or p['lateUpdate']:
            raise ValueError('Unsupported Crumble rotation dispatch')
        go, key = children[names[p['gameObject']['gameObject']['name']]]
        transforms = []
        while key:
            transform = read(key); transforms.append(transform)
            key = f"level6-{transform['m_Father']['m_PathID']}.json" if transform['m_Father']['m_PathID'] else None
        # All authored floor ancestors have zero world rotation. Source sets
        # absolute world Z, retaining X/Y; reflection stays in the local scale.
        for transform in transforms:
            q = transform['m_LocalRotation']
            if any(abs(q['m_'+a]) > 1e-6 for a in 'XYZ'):
                raise ValueError('Crumble floor no longer has identity authored rotation')
        result.append(dict(owner=int(go[7:-5]), pivot=project((0,0,0),transforms)[:2],
                           degrees=p['zAngle']['value'],
                           owners=sorted({int(r['go'][7:-5]) for r in mapping['inventory']
                                          if r['active'] and go in r['owners']})))
    if len(result) != 3:
        raise ValueError('Expected three source Crumble floor poses')
    return result


class OwnerSource:
    def __init__(self, source=SOURCE):
        self.source = source
        self.manifest = json.loads((source / 'manifest.json').read_text())
        if (self.manifest['build'], self.manifest['scene']) != (22529139, 'Tutorial_01'):
            raise ValueError('Unexpected breakable owner source')
        for key, digest in self.manifest['source_sha256'].items():
            if hashlib.sha256((source / key).read_bytes()).hexdigest() != digest:
                raise ValueError('Changed breakable owner source: ' + key)
        self.objects = self.manifest['objects']

    def has(self, key):
        return key in self.manifest['source_sha256']

    def read(self, key):
        return json.loads((self.source / key).read_text())

    def texture(self, key):
        return Image.open(self.source / self.manifest['media'][key]['png']).convert('RGBA')

    def collapse_spawn(self):
        from inspect_fsm import decode_fsm
        fsm = decode_fsm(self.read('level6-12043.json')['m_Structure']['fsm'])
        state = next(s for s in fsm['states'] if s['name'] == 'Break')
        return next(a['parameters'] for a in state['actions'] if a['type'].endswith('.CreateObject'))

    def instance_transform(self, owner, transforms):
        if owner != 'sharedassets6.assets-439.json':
            return transforms
        spawn = self.collapse_spawn()
        if spawn['spawnPoint']['value']['m_PathID']:
            raise ValueError('Unexpected rubble spawn point')
        transforms = copy.deepcopy(transforms)
        position, rotation = spawn['position']['value'], spawn['rotation']['value']
        transforms[0]['m_LocalPosition'] = {'m_'+a.upper(): position[a] for a in 'xyz'}
        x,y,z = (math.radians(rotation[a])*.5 for a in 'xyz')
        cx,sx,cy,sy,cz,sz = math.cos(x),math.sin(x),math.cos(y),math.sin(y),math.cos(z),math.sin(z)
        # Unity Euler order is Y * X * Z.
        transforms[0]['m_LocalRotation'] = dict(m_X=cy*sx*cz+sy*cx*sz, m_Y=sy*cx*cz-cy*sx*sz,
                                               m_Z=cy*cx*sz-sy*sx*cz, m_W=cy*cx*cz+sy*sx*sz)
        return transforms


def extract(port, output=SOURCE):
    from extract_decor_effects import Store, SKIP
    from extract_crawler import Export, native_component_kind
    from extract_plant_hit_dependencies import references
    if (output/'manifest.json').exists() or not output.resolve().is_relative_to((ROOT / '_private/research').resolve()):
        raise ValueError('Use a fresh private source directory')
    output.mkdir(parents=True,exist_ok=True)
    client = Client(port)
    store = Store.__new__(Store)
    store.output, store.client, store.export = output, client, Export(client, output)
    store.cache, store.tables, store.hashes, store.paths, store.media = {}, {}, {}, {}, {}
    store.used, store.used_media = set(), set()
    row = next(r for r in client.search('Collapser Tute') if r['collection'] == 'level6')
    store.register(row['path']['C'])
    objects, visited = {}, set()

    def asset(key):
        if key in visited:
            return
        if len(visited) >= 1024:
            raise ValueError('Breakable presentation closure exceeded bound')
        visited.add(key)
        data = store.get(key)
        if 'm_TextureSettings' in data and 'm_Width' in data:
            store.binary(key, 'texture')
        for location, pointer in references(data):
            field = location.split('/')[1] if '/' in location else location
            if field in SKIP or field in {'m_Children', 'm_Component', 'm_Script'}:
                continue
            target, _ = store.resolve(key, pointer)
            asset(target)

    def transform(key):
        data = store.get(key)
        if data['m_Father']['m_PathID']:
            parent, _ = store.resolve(key, data['m_Father'])
            transform(parent)

    def owner(key):
        data = store.get(key)
        components = {}
        for item in data['m_Components']:
            ck, component = store.resolve(key, item['m_Component'])
            kind = native_component_kind(component)
            if kind == 'Transform':
                transform(ck)
            elif kind in ('ParticleSystem', 'ParticleSystemRenderer'):
                asset(ck)
            else:
                continue
            components.setdefault(kind, []).append(ck)
        objects[key] = dict(name=data['m_Name'], active=data['m_IsActive'], components=components)

    for number in (*COLLAPSE, *(n for children in DOORS.values() for n in children)):
        owner(f'level6-{number}.json')
    floor_transform = store.get('level6-6289.json')
    owner('level6-2421.json')
    store.get('level6-12191.json')
    for child in floor_transform['m_Children']:
        transform_key, transform_data = store.resolve('level6-6289.json', child)
        child_key, child_data = store.resolve(transform_key, transform_data['m_GameObject'])
        print('floor child', child_key, child_data['m_Name'])
        if child_data['m_Name']=='msk_generic':
            owner(child_key)
            for component in child_data['m_Components']:
                store.resolve(child_key,component['m_Component'])
        if child_data['m_Name'].startswith(('Dust Hit ', 'Pt Bits ', 'Pt Wood ')):
            owner(child_key)
    store.get('sharedassets6.assets-107.json')
    audio_path=json.dumps(dict(C=store.paths['sharedassets6.assets'],D=107))
    store.save('floor-hit.ogg',client.request('/Assets/Audio',Path=audio_path))
    owner('sharedassets6.assets-439.json')
    owner('level6-318.json')
    store.get('level6-12043.json')
    # Retain each actual controller, including its authored root scale and angle.
    for number in DOORS:
        key = f'level6-{number}.json'
        owner(key)
        for item in store.get(key)['m_Components']:
            ck, component = store.resolve(key, item['m_Component'])
            if 'debrisParts' in component.get('m_Structure', {}):
                objects[key]['breakable'] = ck
    manifest = dict(build=22529139, scene='Tutorial_01', objects=objects,
                    dependency_tables=store.tables, source_sha256=store.hashes, media=store.media)
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return dict(objects=len(objects), files=len(store.hashes))


def compile_owners(source, emitter, emitters, chain, root_transform, project, read, ambient):
    """Append after existing IDs; the runtime bank maps gameplay owners to children."""
    records = []
    floor_children = {}
    for owner, stage in FLOOR_STAGES.items():
        names = [f'{kind} {stage}' for kind in ('Dust Hit', 'Pt Bits', 'Pt Wood')]
        floor_children[owner] = tuple(int(next(k for k,v in source.objects.items() if v['name']==name)[7:-5]) for name in names)
    for owner, children in [(318, COLLAPSE), *DOORS.items(), *floor_children.items()]:
        angle = 0
        if owner in DOORS:
            root = f'level6-{owner}.json'
            data = read(source.objects[root]['breakable'])['m_Structure']
            angle = data['angleOffset']
            if read(root_transform(root))['m_LocalScale']['m_X'] < 0:
                angle = -angle
        ids = [emitter(f'level6-{number}.json', ambient=ambient,
                       dust='Dust' in source.objects[f'level6-{number}.json']['name'],
                       owned_angle=angle) for number in children]
        if owner == 318:
            ids.append(emitter('sharedassets6.assets-439.json', ambient=ambient, owned_angle=0))
        records.append(dict(owner=owner, emitters=ids))
    binary = bytearray(struct.pack('<4sI', b'OWF2', len(records)))
    for record in records:
        binary.extend(struct.pack('<8I', record['owner'], len(record['emitters']),
                                  *(record['emitters'] + [0] * (6-len(record['emitters'])))))
    ids = [i for r in records for i in r['emitters']]
    binary.extend(struct.pack('<I', len(ids)))
    for i in ids:
        e = emitters[i]
        particle = read(e['source'])
        local = read(source.objects[e['owner']]['components']['Transform'][0])['m_LocalScale']
        # Local scaling affects billboard size; Shape scaling retains size 1.
        size = [abs(local['m_X']), abs(local['m_Y'])] if particle['m_ScalingMode']==1 else [1,1]
        from convert_decor_effects import limits
        force = particle['m_ForceModule']
        force_x = limits(force['m_X']) if force['m_Enabled'] else [0,0]
        binary.extend(struct.pack('<3I6f', i, e['shape'], int(e['visual']), *e['sorting'], *size, *force_x))
    return dict(records=records, pool_bound=sum(emitters[i]['count'] for i in ids), binary=binary)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--extract', action='store_true')
    parser.add_argument('--port', type=int, default=58291)
    args = parser.parse_args()
    print(extract(args.port) if args.extract else len(OwnerSource().objects))
