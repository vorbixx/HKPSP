"""Export Tutorial_01 SpriteRenderer scene and dependencies using AssetRipper.

Original meshes, material/shader data, transforms and pixels stay private.
This is a serialized scene snapshot, not a claim that Start/FSM effects ran.
"""
import argparse
import hashlib
import json
from pathlib import Path
from assetripper_client import Client, ROOT
from extract_crawler import Export, write_asset
from extract_room_tilemap import extract_tilemap
from room_blur import extract_blur_planes
from breakable_prefabs import collect_hierarchies, export_prefab_assets
from breakable_effect_assets import export_effect_assets


def extract(client, output, collection='level6', scene='Tutorial_01'):
    if not any(output.resolve().is_relative_to(p.resolve()) for p in (ROOT/'_private', ROOT/'_private/research')):
        raise ValueError('Export must remain private')
    output.mkdir(parents=True, exist_ok=True)
    scene_collection=collection
    e = Export(client, output)
    inventory, sprites, textures = [], {}, {}
    owner_components = {}
    dependencies = collect_hierarchies(e, scene_collection)
    debris_roots = dependencies['debris']
    breakable_objects, breakable_references = dependencies['objects'], dependencies['references']
    prefab_hierarchies = dependencies['prefabs']
    rows = [r for r in client.search('SpriteRenderer') if r['collection'] == scene_collection]
    for index, row in enumerate(rows):
        collection = row['path']['C']
        renderer, key = e.fetch(collection, row['path']['D'])
        go, go_key, gc = e.resolve(collection, renderer['m_GameObject'])
        parts = e.components(gc, renderer['m_GameObject']['m_PathID'])
        if 'Rigidbody2D' in parts:
            for kind in ('BoxCollider2D', 'PolygonCollider2D', 'CircleCollider2D'):
                for collider in parts.get(kind, []):
                    ref = e.cache[collider].get('m_Material', {})
                    if ref.get('m_PathID'):
                        e.resolve(gc, ref)
        chain, owners = [], []
        transform_key, tc = parts['Transform'][0], gc
        while True:
            transform = e.cache[transform_key]
            chain.append(transform_key)
            parent, owner, owner_collection = e.resolve(tc, transform['m_GameObject'])
            owners.append(owner)
            if owner not in owner_components:
                # A renderer-free parent can deactivate the entire building.
                owner_components[owner] = e.components(owner_collection, transform['m_GameObject']['m_PathID'])
            if not transform['m_Father']['m_PathID']:
                break
            _, transform_key, tc = e.resolve(tc, transform['m_Father'])
            if transform_key in chain:
                raise ValueError('Cyclic transform')
        active = bool(renderer['m_Enabled']) and all(e.cache[k]['m_IsActive'] for k in owners)
        sprite_key = None
        # Start/FSM activation can enable serialized-inactive owners later.
        # Keep their original dependencies; the converter decides visibility.
        if renderer['m_Sprite']['m_PathID']:
            sprite, sprite_key, sc = e.resolve(collection, renderer['m_Sprite'])
            if sprite_key not in sprites:
                texture, texture_key, texc = e.resolve(sc, sprite['m_RD']['m_Texture'])
                if texture_key not in textures:
                    target = texture_key.removesuffix('.json') + '.png'
                    raw = client.request('/Assets/Image', Path=json.dumps({'C':texc,'D':sprite['m_RD']['m_Texture']['m_PathID']}), Extension='png')
                    write_asset(output / target, raw)
                    e.hashes[target] = hashlib.sha256(raw).hexdigest()
                    textures[texture_key] = target
                sprites[sprite_key] = textures[texture_key]
        materials = []
        for ref in renderer['m_Materials']:
            if ref['m_PathID']:
                material, mk, mc = e.resolve(collection, ref)
                materials.append(mk)
                if material['m_Shader']['m_PathID']:
                    e.resolve(mc, material['m_Shader'])
        inventory.append(dict(renderer=key, go=go_key, active=active, chain=chain, owners=owners,
                              sprite=sprite_key, materials=materials, components=parts))
        if index % 500 == 0:
            print(json.dumps(dict(renderers=index, sprites=len(sprites), textures=len(textures))), flush=True)
    breakable_prefabs = export_prefab_assets(e, prefab_hierarchies)
    breakable_prefabs['effects'] = export_effect_assets(e, dependencies)
    mesh_rows = [r for r in client.search('MeshRenderer') if r['collection']==scene_collection]
    mesh_audit = []
    for row in mesh_rows:
        renderer, key = e.fetch(row['path']['C'], row['path']['D'])
        go, go_key, gc = e.resolve(row['path']['C'], renderer['m_GameObject'])
        parts = e.components(gc, renderer['m_GameObject']['m_PathID'])
        mesh_audit.append(dict(renderer=key, go=go_key, name=go['m_Name'],
                               serialized_enabled=bool(renderer['m_Enabled']), components=parts))
    tilemap = extract_tilemap(e, scene_collection)
    blur_planes = extract_blur_planes(e, scene_collection)
    manifest = dict(build=22529139, scene=scene, inventory=inventory, tilemap=tilemap, blur_planes=blur_planes,
                    sprites=sprites, source_sha256=e.hashes, owner_components=owner_components,
                    breakable_debris_roots=sorted(debris_roots),
                    breakable_objects=list(breakable_objects.values()),
                    breakable_references=breakable_references,
                    breakable_prefabs=breakable_prefabs,
                    collection_dependencies={name: {str(i): label for i, (_, label) in deps.items()}
                                             for name, deps in e.collections.values()},
                    mesh_renderers=mesh_rows, mesh_renderer_audit=mesh_audit)
    (output / 'room-visuals-map.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    print(json.dumps(dict(renderers=len(rows), sprites=len(sprites), textures=len(textures))))


def supplement_owner_components(client, output):
    """Upgrade an existing scene export without re-exporting its textures."""
    if not any(output.resolve().is_relative_to(p.resolve())
               for p in (ROOT / '_private', ROOT / '_private/research')):
        raise ValueError('Export must remain private')
    manifest_path = output / 'room-visuals-map.json'
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    e = Export(client, output)
    for ref, expected in manifest['source_sha256'].items():
        if not ref.endswith('.json'):
            continue
        raw = (output / ref).read_bytes()
        if hashlib.sha256(raw).hexdigest() != expected:
            raise ValueError(f'Changed source export: {ref}')
        e.cache[ref] = json.loads(raw)
    collections, components = {}, {}
    for owner in sorted({ref for row in manifest['inventory'] for ref in row['owners']}):
        collection_name, ident = Path(owner).stem.rsplit('-', 1)
        if collection_name not in collections:
            name = json.loads((output / owner).read_text(encoding='utf-8'))['m_Name']
            rows = [row for row in client.search(name) if row['collection'] == collection_name
                    and row['type'] == 'GameObject' and row['path']['D'] == int(ident)]
            if len(rows) != 1:
                raise ValueError(f'Cannot resolve exported owner {owner} in loaded source')
            collections[collection_name] = rows[0]['path']['C']
        components[owner] = e.components(collections[collection_name], int(ident))
    manifest['owner_components'] = components
    manifest['source_sha256'].update(e.hashes)
    write_asset(manifest_path, json.dumps(manifest, indent=2).encode('utf-8'))
    print(json.dumps(dict(owners=len(components), files=len(e.hashes))))


def has_breakable_export(mapping):
    """Empty banks are complete too; older renderer-only exports must upgrade."""
    if not isinstance(mapping, dict):
        return False
    bank = mapping.get('breakable_prefabs')
    effects = bank.get('effects') if isinstance(bank, dict) else None
    return (isinstance(mapping.get('breakable_objects'), list)
            and isinstance(mapping.get('breakable_references'), list)
            and isinstance(mapping.get('breakable_debris_roots'), list)
            and isinstance(bank, dict)
            and isinstance(bank.get('inventory'), list)
            and all(isinstance(bank.get(name), dict) for name in ('roots', 'sprites', 'textures'))
            and isinstance(effects, dict)
            and all(isinstance(effects.get(name), dict)
                    for name in ('media', 'particles', 'audio_cues')))


def supplement_breakables(client, output, collection):
    """Upgrade saved dependencies without re-exporting the room's artwork."""
    if not any(output.resolve().is_relative_to(p.resolve())
               for p in (ROOT / '_private', ROOT / '_private/research')):
        raise ValueError('Export must remain private')
    manifest_path = output / 'room-visuals-map.json'
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    if not any(row['go'].rsplit('-', 1)[0] == collection for row in manifest['inventory']):
        raise ValueError('Collection does not match the saved room: ' + collection)
    for name, expected in manifest['source_sha256'].items():
        if hashlib.sha256((output / name).read_bytes()).hexdigest() != expected:
            raise ValueError('Changed source export: ' + name)
    e = Export(client, output)
    dependencies = collect_hierarchies(e, collection)
    bank = export_prefab_assets(e, dependencies['prefabs'])
    bank['effects'] = export_effect_assets(e, dependencies)
    manifest.update(breakable_objects=list(dependencies['objects'].values()),
                    breakable_references=dependencies['references'], breakable_prefabs=bank,
                    breakable_debris_roots=sorted(dependencies['debris']))
    manifest.setdefault('collection_dependencies', {}).update(
        {name: {str(i): label for i, (_, label) in deps.items()}
         for name, deps in e.collections.values()})
    manifest['source_sha256'].update(e.hashes)
    write_asset(manifest_path, json.dumps(manifest, indent=2).encode('utf-8'))
    print(json.dumps(dict(objects=len(dependencies['objects']),
                         references=len(dependencies['references']), templates=len(bank['roots']))))


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port', type=int, default=58291)
    p.add_argument('--output', type=Path, default=ROOT / '_private/research/room-visuals')
    p.add_argument('--collection', default='level6')
    p.add_argument('--scene', default='Tutorial_01')
    mode = p.add_mutually_exclusive_group()
    mode.add_argument('--owner-components-only', action='store_true')
    mode.add_argument('--breakables-only', action='store_true')
    a = p.parse_args()
    if a.owner_components_only:
        supplement_owner_components(Client(a.port), a.output)
    elif a.breakables_only:
        supplement_breakables(Client(a.port), a.output, a.collection)
    else:
        extract(Client(a.port), a.output, a.collection, a.scene)
