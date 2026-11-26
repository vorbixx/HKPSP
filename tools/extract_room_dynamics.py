"""Targeted, hash-anchored Cave Spikes controller/clip/sprite export."""
import hashlib
import json

from assetripper_client import Client, ROOT
from extract_crawler import Export


def extract():
    source = ROOT / '_private/research/room-visuals'
    output = ROOT / '_private/research/room-dynamics'
    output.mkdir(parents=True, exist_ok=True)
    mapping = json.loads((source / 'room-visuals-map.json').read_bytes())
    client = Client()
    live = next(r for r in client.search('Cave Spikes')
                if r['collection'] == 'level6' and r['type'] == 'GameObject')
    collection = live['path']['C']
    export = Export(client, output)
    owners, textures, anchors = [], {}, {}
    for row in mapping['inventory']:
        go = json.loads((source / row['go']).read_bytes())
        if not row['active'] or not go['m_Name'].startswith('Cave Spikes'):
            continue
        animators = []
        for component in go['m_Components']:
            key = f"level6-{component['m_Component']['m_PathID']}.json"
            raw = (source / key).read_bytes()
            if hashlib.sha256(raw).hexdigest() != mapping['source_sha256'][key]:
                raise ValueError('Changed room anchor: ' + key)
            data = json.loads(raw)
            if 'm_Controller' in data:
                animators.append((key, data))
        if len(animators) != 1:
            raise ValueError('Expected one spike Animator')
        key, animator = animators[0]
        anchors[key] = mapping['source_sha256'][key]
        controller, controller_key, cc = export.resolve(collection, animator['m_Controller'])
        clips = []
        for ref in controller['m_AnimationClips']:
            clip, clip_key, clip_collection = export.resolve(cc, ref)
            clips.append(clip_key)
            for curve in clip['m_PPtrCurves']:
                for frame in curve['m_Curve']:
                    sprite, _, sc = export.resolve(clip_collection, frame['m_Value'])
                    texture, texture_key, tc = export.resolve(sc, sprite['m_RD']['m_Texture'])
                    if texture_key not in textures:
                        image_name = texture_key.removesuffix('.json') + '.png'
                        image = client.request('/Assets/Image',
                            Path=json.dumps({'C': tc, 'D': sprite['m_RD']['m_Texture']['m_PathID']}),
                            Extension='png')
                        (output / image_name).write_bytes(image)
                        export.hashes[image_name] = hashlib.sha256(image).hexdigest()
                        textures[texture_key] = image_name
        owners.append(dict(owner=int(row['go'][7:-5]), animator=key,
                           controller=controller_key, clips=clips))
    if len(owners) != 32:
        raise ValueError('Expected 32 spike owners')
    rock_materials, rock_children = {}, {}
    for row in mapping['inventory']:
        if 'SimpleRock' not in row['components']:
            continue
        for key in row['components']['BoxCollider2D']:
            collider = json.loads((source/key).read_bytes())
            if 'm_IsTrigger' in collider and not collider['m_IsTrigger']:
                _, material, _ = export.resolve(collection, collider['m_Material'])
                rock_materials[row['go']] = material
        root_transform = json.loads((source/row['chain'][0]).read_bytes())
        child_keys = []
        for child in root_transform['m_Children']:
            transform, key, _ = export.resolve(collection, child)
            child_keys.append(key)
            go, key, _ = export.resolve(collection, transform['m_GameObject'])
            child_keys.append(key)
            for entry in go['m_Components']:
                _, key, _ = export.resolve(collection, entry['m_Component'])
                if key not in child_keys:
                    child_keys.append(key)
        rock_children[row['go']] = child_keys
    report = dict(build=mapping['build'], scene=mapping['scene'], spikes=owners,
                  rock_materials=rock_materials, rock_children=rock_children,
                  textures=textures, source_sha256=export.hashes, room_anchors=anchors,
                  room_manifest_sha256=hashlib.sha256((source/'room-visuals-map.json').read_bytes()).hexdigest())
    (output / 'manifest.json').write_text(json.dumps(report, indent=2)+'\n')
    print(f'Exported {len(owners)} spikes; {len(export.hashes)} hashed dependencies')


def extract_crumble():
    """Supplement the saved FSM with the actual direct dust child and components."""
    from extract_decor_effects import Store
    from breakable_owner_effects import OwnerSource
    output = ROOT/'_private/research/collapse-crumble'
    output.mkdir(parents=True,exist_ok=True)
    store = Store(output,Client())
    owner = OwnerSource()
    root = owner.objects['level6-318.json']['components']['Transform'][0]
    for key in (root,'level6-12043.json'):
        store.get(key)
        if store.hashes[key] != owner.manifest['source_sha256'][key]:
            raise ValueError('Live Crumble source differs from the checked owner export: '+key)
    children = []
    for pointer in owner.read(root)['m_Children']:
        key = f"level6-{pointer['m_PathID']}.json"
        transform = store.get(key)
        go,data = store.resolve(key,transform['m_GameObject'])
        children.append(dict(owner=go,name=data['m_Name'],transform=key))
        if data['m_Name'] == 'Dust Collapse Large':
            for item in data['m_Components']:
                component,value = store.resolve(go,item['m_Component'])
                if 'm_Script' in value:
                    store.resolve(component,value['m_Script'])
    manifest = dict(build=22529139,scene='Tutorial_01',children=children,
                    controller_sha256=owner.manifest['source_sha256']['level6-12043.json'],
                    source_sha256=store.hashes,dependency_tables=store.tables)
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(f'Crumble supplement: {len(children)} direct children, {len(store.hashes)} hashed records')


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--crumble',action='store_true')
    if parser.parse_args().crumble:
        extract_crumble()
    else:
        extract()
