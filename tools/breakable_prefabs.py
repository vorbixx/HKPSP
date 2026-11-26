"""Export shared breakable render assets without inventing scene instances."""
import hashlib
import json

from extract_crawler import write_asset
from breakable_hierarchy import hierarchy


ACTIVATION_FIELDS = ('wholeParts', 'remnantParts', 'debrisParts', 'flingDebris')
SPAWN_FIELDS = ('audioSourcePrefab', 'dustHitRegularPrefab', 'dustHitDownPrefab',
                'strikeEffectPrefab', 'nailHitEffectPrefab', 'spellHitEffectPrefab',
                'objectNailEffectPrefab', 'midpointNailEffectPrefab',
                'containingParticles')


def prefab_references(data):
    """Keep weighted entries and their indices; export does not choose a spawn."""
    def refs(value, path):
        if isinstance(value, dict):
            if set(value) == {'m_FileID', 'm_PathID'}:
                if value['m_PathID']:
                    yield path, value
            else:
                for name, child in value.items():
                    yield from refs(child, path + '/' + name)
        elif isinstance(value, list):
            for index, child in enumerate(value):
                yield from refs(child, path + '/' + str(index))

    for field in (*ACTIVATION_FIELDS, *SPAWN_FIELDS):
        for path, ref in refs(data.get(field), field):
            yield field, path, ref


def collect_hierarchies(export, scene_collection):
    """Collect activation and spawn dependencies for full and incremental exports."""
    objects, references, prefabs = {}, [], {}
    remnants, debris, sources = set(), set(), []
    for row in export.client.search('MonoBehaviour'):
        if row['collection'] != scene_collection:
            continue
        component, key = export.fetch(row['path']['C'], row['path']['D'])
        if not component.get('m_Script', {}).get('m_PathID'):
            continue
        script, _, _ = export.resolve(row['path']['C'], component['m_Script'])
        if script.get('m_ClassName') not in ('Breakable', 'BreakableObject'):
            continue
        sources.append(dict(component=key, collection=row['path']['C']))
        source_owner = None
        owner_ref = component.get('m_GameObject', {})
        if owner_ref.get('m_PathID'):
            _, source_owner, _ = export.resolve(row['path']['C'], owner_ref)
        for field, path, ref in prefab_references(component.get('m_Structure', {})):
            asset, reference, rc = export.resolve(row['path']['C'], ref)
            root, root_id = reference, ref['m_PathID']
            # AudioSource/ParticleSystem fields reference components, not GameObjects.
            if 'm_Components' not in asset:
                owner = asset.get('m_GameObject', {})
                if not owner.get('m_PathID'):
                    raise ValueError('Breakable prefab has no GameObject: ' + reference)
                asset, root, rc = export.resolve(rc, owner)
                root_id = owner['m_PathID']
                if 'm_Components' not in asset:
                    raise ValueError('Breakable prefab owner is not a GameObject: ' + root)
            local = export.collection(rc)[0] == scene_collection
            if field == 'remnantParts':
                remnants.add(root)
            if field in ('debrisParts', 'flingDebris'):
                debris.add(root)
            if root not in objects:
                for item in hierarchy(export, rc, root_id):
                    item['scene_instance'] = local
                    objects[item['go']] = item
            mode = 'activate' if field in ACTIVATION_FIELDS else 'spawn'
            # Breakable.Break activates direct children; other weighted objects spawn.
            if field == 'containingParticles' and source_owner and objects[root]['owners'][1:2] == [source_owner]:
                mode = 'activate'
            references.append(dict(component=key, field=field, path=path, reference=reference, root=root,
                                   scene_instance=local, mode=mode))
            if not local or mode == 'spawn':
                prefabs[root] = [item for item in objects.values() if root in item['owners']]
    return dict(objects=objects, references=references, prefabs=prefabs,
                remnants=remnants, debris=debris, sources=sources)


def export_prefab_assets(export, hierarchies):
    """A template bank uses fully qualified source IDs and no room renderer IDs.

    Sprite/texture tables remain separate from the ordinary room atlas inputs.
    Placement and activation must be supplied by the owning runtime component.
    """
    roots, inventory, sprites, textures = {}, {}, {}, {}
    for root, rows in sorted(hierarchies.items()):
        roots[root] = []
        for row in rows:
            collection = row['collection']
            for key in row['components'].get('SpriteRenderer', []):
                roots[root].append(key)
                if key in inventory:
                    continue
                renderer = export.cache[key]
                sprite_key = None
                sprite_ref = renderer.get('m_Sprite', {})
                if sprite_ref.get('m_PathID'):
                    sprite, sprite_key, sc = export.resolve(collection, sprite_ref)
                    if sprite_key not in sprites:
                        texture_ref = sprite['m_RD']['m_Texture']
                        _, texture_key, tc = export.resolve(sc, texture_ref)
                        if texture_key not in textures:
                            target = texture_key.removesuffix('.json') + '.png'
                            raw = export.client.request('/Assets/Image',
                                Path=json.dumps({'C': tc, 'D': texture_ref['m_PathID']}), Extension='png')
                            write_asset(export.output / target, raw)
                            export.hashes[target] = hashlib.sha256(raw).hexdigest()
                            textures[texture_key] = target
                        sprites[sprite_key] = textures[texture_key]
                materials = []
                for ref in renderer.get('m_Materials', []):
                    if not ref.get('m_PathID'):
                        continue
                    material, material_key, mc = export.resolve(collection, ref)
                    materials.append(material_key)
                    shader = material.get('m_Shader', {})
                    if shader.get('m_PathID'):
                        export.resolve(mc, shader)
                inventory[key] = dict(renderer=key, go=row['go'], chain=row['chain'],
                    owners=row['owners'], components=row['components'], sprite=sprite_key,
                    materials=materials, scene_instance=False,
                    active=bool(renderer.get('m_Enabled', True)) and
                           all(export.cache[k]['m_IsActive'] for k in row['owners']))
        roots[root] = sorted(set(roots[root]))
    return dict(roots=roots, inventory=[inventory[k] for k in sorted(inventory)],
                sprites=sprites, textures=textures)
