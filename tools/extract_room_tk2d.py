"""Supplement saved room artwork with hash-anchored, non-actor tk2d meshes.

Run under validation-lock.ps1 -Exclusive. Source assets remain private. Runtime
activation, collision, animation clocks and progression are not changed here.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path

from assetripper_client import Client, ROOT
from extract_crawler import Export, write_asset


from room_tk2d_artwork import runtime_owned
from scene_levers import controllers


def extract(client, source, *, only_owners=None):
    source = source.resolve()
    if not any(source.is_relative_to(root.resolve()) for root in
               (ROOT / '_private/research', ROOT / '_private/minimum-obstacles')):
        raise ValueError('Expected a saved private scene visual export')
    raw = (source / 'room-visuals-map.json').read_bytes()
    mapping = json.loads(raw)
    pairs = controllers(mapping, lambda n: json.loads((source / n).read_bytes()))
    controlled = {p[k]['go'] for p in pairs for k in ('lever','gate')}
    rows = [r for r in mapping.get('mesh_renderer_audit', [])
            if 'tk2dSprite' in r['components'] and
            (only_owners is None or r['go'] in only_owners)]
    output = source / 'tk2d'
    output.mkdir(exist_ok=True)
    previous = json.loads((output / 'manifest.json').read_bytes()) if only_owners and (output / 'manifest.json').is_file() else None
    export = Export(client, output)
    entries, sprites, owners, rejected = [], {}, {}, []

    def save(name, data):
        raw = json.dumps(data, indent=2).encode()
        write_asset(output / name, raw)
        export.hashes[name] = hashlib.sha256(raw).hexdigest()
        return 'tk2d/' + name

    def local(key):
        if key in mapping['source_sha256']:
            expected = mapping['source_sha256'][key]
            if key in export.hashes and export.hashes[key] != expected:
                raise ValueError('Changed tk2d source anchor: ' + key)
            return key
        return 'tk2d/' + key

    collections = {}
    for row in rows:
        name = row['go'].rsplit('-', 1)[0]
        if name not in collections:
            hits = [r for r in client.search(row['name'])
                    if r['collection'] == name and r['type'] == 'GameObject']
            if not hits:
                raise ValueError('Load original scene collection: ' + name)
            collections[name] = hits[0]['path']['C']
        collection = collections[name]
        parts = row['components']
        renderer = json.loads((source / row['renderer']).read_bytes())
        tk = json.loads((source / parts['tk2dSprite'][0]).read_bytes())['m_Structure']
        chain, owner_chain, actor = [], [], False
        row_owners = {}
        transform_key = parts['Transform'][0]
        tc = collection
        active = bool(renderer['m_Enabled'])
        while True:
            transform, key = export.fetch(tc, int(Path(transform_key).stem.rsplit('-', 1)[1]))
            chain.append(local(key))
            go, go_key, gc = export.resolve(tc, transform['m_GameObject'])
            owner_chain.append(local(go_key))
            components = export.components(gc, transform['m_GameObject']['m_PathID'])
            row_owners[local(go_key)] = {k: [local(v) for v in values] for k, values in components.items()}
            if go_key not in controlled:
                actor |= runtime_owned(components, lambda ref: json.loads((output / ref).read_bytes()))
            active &= bool(go['m_IsActive'])
            if not transform['m_Father']['m_PathID']:
                break
            _, transform_key, tc = export.resolve(tc, transform['m_Father'])
            if local(transform_key) in chain:
                raise ValueError('Cyclic tk2d transform')
        if actor:
            rejected.append(dict(renderer=row['renderer'], reason='Actor/runtime-owned hierarchy'))
            continue
        collection_data, collection_key, cc = export.resolve(collection, tk['collection'])
        data = collection_data['m_Structure']
        sprite_id = tk['_spriteId']
        definition = copy.deepcopy(data['spriteDefinitions'][sprite_id])
        if not definition['positions'] or not definition['uvs']:
            rejected.append(dict(renderer=row['renderer'], reason='Empty source sprite definition'))
            continue
        if definition['complexGeometry'] or len(definition['positions']) != 4:
            raise ValueError('Unsupported tk2d room mesh: ' + definition['name'])
        material_id = definition['materialId']
        texture, texture_key, texture_collection = export.resolve(cc, data['textures'][material_id])
        png = texture_key.removesuffix('.json') + '.png'
        if png not in export.hashes:
            image = client.request('/Assets/Image', Path=json.dumps(
                {'C': texture_collection, 'D': data['textures'][material_id]['m_PathID']}), Extension='png')
            write_asset(output / png, image)
            export.hashes[png] = hashlib.sha256(image).hexdigest()
        definition['materialId'] = 0  # The selected source texture is now explicit.
        sprite_name = collection_key.removesuffix('.json') + '-sprite-' + str(sprite_id) + '.json'
        sprite = save(sprite_name, {'_tk2d_definition': definition, 'm_PixelsToUnits': 1})
        sprites[sprite] = 'tk2d/' + png
        materials = []
        for ref in renderer['m_Materials']:
            if ref['m_PathID']:
                _, key, _ = export.resolve(collection, ref)
                materials.append(local(key))
        normalized = copy.deepcopy(renderer)
        normalized.update(m_Color={'m_' + c.upper(): tk['_color'][c] for c in 'rgba'},
                          m_FlipX=False, m_FlipY=False)
        normalized_key = save(row['renderer'].removesuffix('.json') + '-sprite-renderer.json', normalized)
        scale_key = save(row['renderer'].removesuffix('.json') + '-sprite-scale.json', dict(
            m_LocalPosition=dict(m_X=0, m_Y=0, m_Z=0),
            m_LocalRotation=dict(m_X=0, m_Y=0, m_Z=0, m_W=1),
            m_LocalScale={'m_' + c.upper(): tk['_scale'][c] for c in 'xyz'}))
        owners.update(row_owners)
        entries.append(dict(renderer=normalized_key, go=row['go'], active=active,
                            chain=[scale_key] + chain, owners=owner_chain, sprite=sprite,
                            materials=materials, components=parts, _tk2d_mesh=row['renderer']))
    bouncers = []
    for row in rows:
        if 'BounceShroom' not in row['components']:
            continue
        collection = collections[row['go'].rsplit('-',1)[0]]
        animator = json.loads((source / row['components']['tk2dSpriteAnimator'][0]).read_bytes())['m_Structure']
        _, library, _ = export.resolve(collection, animator['library'])
        script = json.loads((source / row['components']['BounceShroom'][0]).read_bytes())['m_Structure']
        bouncers.append(dict(owner=row['go'],library='tk2d/'+library,
                             idle=script['idleAnim'],bounce=script['bounceAnim']))
    levers = []
    for pair in pairs:
        gate = pair['gate'];collection = collections[gate['go'].rsplit('-',1)[0]]
        animator = json.loads((source / gate['components']['tk2dSpriteAnimator'][0]).read_bytes())['m_Structure']
        _, library, _ = export.resolve(collection, animator['library'])
        entry=dict(owner=pair['lever']['go'],gate=gate['go'],library='tk2d/' + library)
        if pair['pillar']:
            switch=pair['lever']
            animator=json.loads((source/switch['components']['tk2dSpriteAnimator'][0]).read_bytes())['m_Structure']
            _, switch_library, _=export.resolve(collection,animator['library'])
            entry.update(pillar=True,switch_library='tk2d/'+switch_library)
        levers.append(entry)
    manifest = dict(build=mapping['build'], scene=mapping['scene'],levers=levers,bouncers=bouncers,
                    room_manifest_sha256=hashlib.sha256(raw).hexdigest(),
                    inventory=entries, sprites=sprites, owner_components=owners,
                    source_sha256=export.hashes, rejected=rejected,
                    limitations=['Serialized initial tk2d pose; runtime clip/state animation remains separate.'])
    if previous:
        manifest['inventory'] = [r for r in previous['inventory'] if r['go'] not in only_owners] + entries
        for field in ('sprites', 'owner_components', 'source_sha256'):
            manifest[field] = {**previous[field], **manifest[field]}
        manifest['levers'] = [r for r in previous.get('levers', []) if r['owner'] not in only_owners] + levers
        manifest['bouncers'] = [r for r in previous.get('bouncers', []) if r['owner'] not in only_owners] + bouncers
    write_asset(output / 'manifest.json', json.dumps(manifest, indent=2).encode())
    print(json.dumps(dict(scene=mapping['scene'], tk2d_sprites=len(sprites),
                          tk2d_renderers=len(entries), rejected=len(rejected))), flush=True)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, action='append', required=True)
    p.add_argument('--port', type=int, default=58291)
    a = p.parse_args()
    for source in a.source:
        extract(Client(a.port), source)
