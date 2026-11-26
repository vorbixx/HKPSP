"""Export verified static Breakable decor; original values stay in private files.

Animator-bearing plants are explicitly excluded. No particle, audio or save
dependencies are represented by this collision/renderer-activation package.
"""
import converted_files  # Install hardlink copy-on-write before asset writes.
import hashlib
import json
import shutil
import struct
from pathlib import Path

from assetripper_client import ROOT
from convert_room_visuals import project
from extract_crawler import native_component_kind


def observe(source):
    manifest_bytes = (source / 'room-visuals-map.json').read_bytes()
    manifest = json.loads(manifest_bytes)
    if (manifest['build'], manifest['scene']) != (22529139, 'Tutorial_01'):
        raise ValueError('Unexpected decor build/scene')
    hashes, cache = {}, {}

    def read(key):
        if Path(key).name != key or key not in manifest['source_sha256']:
            raise ValueError('Untracked decor dependency: ' + key)
        if key not in cache:
            raw = (source / key).read_bytes()
            digest = hashlib.sha256(raw).hexdigest()
            if digest != manifest['source_sha256'][key]:
                raise ValueError('Changed decor dependency: ' + key)
            cache[key] = json.loads(raw)
            hashes[key] = digest
        return cache[key]

    def require(ok, message):
        if not ok:
            raise ValueError(message)

    def local_ref(ref):
        require(ref['m_FileID'] == 0 and ref['m_PathID'] > 0, 'Expected local decor reference')
        return f"level6-{ref['m_PathID']}.json"

    records, excluded, seen = [], [], set()
    inventory = {r['go']: r for r in manifest['inventory']}
    for row in manifest['inventory']:
        if row['go'] in seen or 'Breakable' not in row['components']:
            continue
        seen.add(row['go'])
        go = read(row['go'])
        if go['m_Layer'] != 19:
            continue
        require(len(row['components']['Breakable']) == 1, 'Multiple Breakable components')
        component_key = row['components']['Breakable'][0]
        component = read(component_key)
        data = component['m_Structure']
        chain = [read(k) for k in row['chain']]
        origin = project((0, 0, 0), chain)
        if not row['active'] or not component['m_Enabled'] or not (
                data['inertForegroundThreshold'] <= origin[2] <= data['inertBackgroundThreshold']):
            continue
        if 'PlayFromRandomFrameMecanim' in row['components']:
            # Still verify all root components, including the unclassified Animator.
            for entry in go['m_Components']:
                read(local_ref(entry['m_Component']))
            excluded.append(dict(owner=int(row['go'][7:-5]), reason='Animator dependency unresolved'))
            continue
        require(go['m_TagString'] == 'Untagged', 'Unsupported decor tag')
        require(not data['wholeParts'] and len(data['remnantParts']) == 1 and
                not data['forwardBreakEvent'] and not data['hitEventReciever']['m_PathID'],
                'Unsupported static activation/event topology')
        renderer_key = local_ref(data['wholeRenderer'])
        require(renderer_key == row['renderer'] and read(renderer_key)['m_Enabled'], 'Whole renderer mismatch')
        boxes = []
        for entry in go['m_Components']:
            key = local_ref(entry['m_Component'])
            kind = native_component_kind(read(key))
            require(key == component_key or kind in ('Transform', 'SpriteRenderer', 'BoxCollider2D'),
                    'Unresolved decor root component: ' + key)
            if kind == 'BoxCollider2D':
                boxes.append((key, read(key)))
        require(len(boxes) == 1, 'Expected one decor collider')
        box_key, box = boxes[0]
        require(box['m_Enabled'] and box['m_IsTrigger'] and box['m_EdgeRadius'] == 0,
                'Unsupported decor collider branch')
        offset, size = box['m_Offset'], box['m_Size']
        corners = [project((offset['m_X'] + x*size['m_X']/2,
                            offset['m_Y'] + y*size['m_Y']/2, 0), chain)
                   for x, y in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
        bounds = [min(p[0] for p in corners), min(p[1] for p in corners),
                  max(p[0] for p in corners), max(p[1] for p in corners)]
        require(all(abs(p[2]-origin[2]) < 1e-5 for p in corners), 'Tilted decor collider needs 3D handling')
        remnant_key = local_ref(data['remnantParts'][0])
        remnant = read(remnant_key)
        require(remnant_key in inventory, 'Remnant missing from artwork inventory')
        remnant_row = inventory[remnant_key]
        require(remnant['m_IsActive'] and remnant_row['active'] and remnant_row['sprite'] and
                read(remnant_row['renderer'])['m_Enabled'], 'Unsupported remnant activation')
        for entry in remnant['m_Components']:
            key = local_ref(entry['m_Component'])
            require(native_component_kind(read(key)) in ('Transform', 'SpriteRenderer'),
                    'Unresolved remnant component: ' + key)
        remnant_transform = read(remnant_row['components']['Transform'][0])
        require(not remnant_transform['m_Children'], 'Remnant children need activation traversal')
        for transform in chain:
            require(read(local_ref(transform['m_GameObject']))['m_IsActive'], 'Inactive decor ancestor')
        records.append(dict(whole=int(row['go'][7:-5]), remnant=int(remnant_key[7:-5]),
                            layer=go['m_Layer'], bounds=bounds, vertices=[list(p[:2]) for p in corners], collider=box_key,
                            component=component_key, z=origin[2]))
    require(records and len(records) <= 32, 'Invalid decor count')
    ids = [v[k] for v in records for k in ('whole', 'remnant')]
    require(len(set(ids)) == len(ids), 'Overlapping decor ownership')
    return dict(build=manifest['build'], scene=manifest['scene'], records=records,
                excluded=excluded, source_sha256=hashes,
                manifest_sha256=hashlib.sha256(manifest_bytes).hexdigest())


def convert():
    report = observe(ROOT / '_private/research/room-visuals')
    behavior_path = ROOT / '_private/research/decompiled/all/Breakable.cs'
    behavior_hash = hashlib.sha256(behavior_path.read_bytes()).hexdigest()
    if behavior_hash != '36c7acbd5a52d4ad75eac839a15235b617e3ee41ed50b534425aac91fee6779a':
        raise ValueError('Breakable behavior source changed; recheck activation contract')
    report['behavior_source_sha256'] = behavior_hash
    binary = bytearray(struct.pack('<4sI', b'HKB1', len(report['records'])))
    for row in report['records']:
        binary.extend(struct.pack('<3I8f', row['whole'], row['remnant'], row['layer'],
                                  *(value for point in row['vertices'] for value in point)))
    output = ROOT / '_private/converted/decor'
    output.mkdir(parents=True, exist_ok=True)
    (output / 'decor.bin').write_bytes(binary)
    shutil.copyfile(output / 'decor.bin', ROOT / '_private/converted/hero/romfs/decor.bin')
    report['binary_sha256'] = hashlib.sha256(binary).hexdigest()
    report['limitations'] = ['Static whole/remnant activation and nail trigger only.',
                             'No debris, dust, sound, camera shake or matched visual acceptance.',
                             'Animator-bearing plants excluded.']
    (output / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(dict(decor=len(report['records']), excluded=len(report['excluded']), bytes=len(binary))))


if __name__ == '__main__':
    convert()
