"""Inventory Tutorial_01 colliders and export a static Terrain snapshot privately.

The region is a research selection window, not a room bound. Intersecting
colliders are kept whole. Triggers remain queryable. No terrain is inferred
from sprites or replaced with collider bounding boxes.
"""
import argparse
import hashlib
import json
from pathlib import Path
from assetripper_client import Client, ROOT
from extract_crawler import Export
from door_transitions import prompt_binding, prompt_marker


def export_door_markers(export, collection, parts):
    for key in parts.get('PlayMakerFSM', []):
        component = export.cache[key]
        fsm = component.get('m_Structure', {}).get('fsm', {})
        if not component.get('m_Enabled', 1) or fsm.get('name') != 'Door Control':
            continue
        binding = prompt_binding(fsm)
        if not binding:
            continue
        transform = export.cache[parts['Transform'][0]]
        for reference in transform.get('m_Children', []):
            child, _, child_collection = export.resolve(collection, reference)
            export.resolve(child_collection, child['m_GameObject'])
        prompt_marker(key, export.cache.__getitem__, transform, binding['marker_name'])


def extract(client, output, collection='level6', scene='Tutorial_01'):
    if not any(output.resolve().is_relative_to(p.resolve()) for p in (ROOT/'_private', ROOT/'_private/research')):
        raise ValueError('Export must remain private')
    output.mkdir(parents=True, exist_ok=True)
    scene_collection=collection
    e = Export(client, output)
    rows = [r for r in client.search('Collider2D') if r['collection'] == scene_collection]
    if not rows:
        raise ValueError('Load the private Tutorial_01 analysis input first')
    inventory = []
    for row in rows:
        collection = row['path']['C']
        collider, key = e.fetch(collection, row['path']['D'])
        go, go_key, gc = e.resolve(collection, collider['m_GameObject'])
        parts = e.components(gc, collider['m_GameObject']['m_PathID'])
        export_door_markers(e, gc, parts)
        chain, owners, components = [], [], []
        transform_key = parts['Transform'][0]
        tc = gc
        while True:
            transform = e.cache[transform_key]
            chain.append(transform_key)
            owner, owner_key, oc = e.resolve(tc, transform['m_GameObject'])
            owners.append(owner_key)
            # Export every component, including behaviours on ancestors. The
            # converter audits rigidbodies and flags dynamic dependencies.
            components.append(e.components(oc, transform['m_GameObject']['m_PathID']))
            if not transform['m_Father']['m_PathID']:
                break
            transform, transform_key, tc = e.resolve(tc, transform['m_Father'])
            if transform_key in chain:
                raise ValueError('Cyclic Transform hierarchy')
        material = None
        if collider['m_Material']['m_PathID']:
            _, material, _ = e.resolve(collection, collider['m_Material'])
        inventory.append(dict(type=row['type'], collider=key, go=go_key,
                              chain=chain, owners=owners, components=components,
                              material=material))
    settings = next(r for r in client.search('Physics2DSettings') if r['collection']=='globalgamemanagers')
    physics, physics_key = e.fetch(settings['path']['C'], settings['path']['D'])
    _, default_material, _ = e.resolve(settings['path']['C'], physics['m_DefaultMaterial'])
    actor = next((r for r in client.search('Crawler 1') if r['collection']==scene_collection and r['type']=='GameObject' and r['name']=='Crawler 1'), None)
    if actor is None:
        manifest = dict(build=22529139, scene=scene, inventory=inventory,
                        physics=physics_key, default_material=default_material,
                        actor_material=default_material, corpse_material=default_material,
                        source_sha256=e.hashes)
        (output/'terrain-map.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
        return
    parts = e.components(actor['path']['C'], actor['path']['D'])
    death = e.cache[parts['EnemyDeathEffects'][0]]['m_Structure']
    _, _, cc = e.resolve(actor['path']['C'], death['corpsePrefab'])
    corpse = e.components(cc, death['corpsePrefab']['m_PathID'])
    body_materials = []
    for group, col in [(parts, actor['path']['C']), (corpse, cc)]:
        ref = e.cache[group['BoxCollider2D'][0]]['m_Material']
        body_materials.append(e.resolve(col, ref)[1] if ref['m_PathID'] else default_material)
    manifest = dict(build=22529139, scene=scene, inventory=inventory,
                    physics=physics_key, default_material=default_material,
                    actor_material=body_materials[0], corpse_material=body_materials[1],
                    actor_transform=parts['Transform'][0], source_sha256=e.hashes)
    (output/'terrain-map.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    print(json.dumps(dict(colliders=len(rows), files=len(e.hashes))))


if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port',type=int,default=58291)
    p.add_argument('--output',type=Path,default=ROOT/'_private/research/terrain')
    a=p.parse_args(); extract(Client(a.port),a.output)
