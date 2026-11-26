"""Export Knight Focus presentation objects and their sprite dependencies."""
import json
from pathlib import Path

from assetripper_client import Client, ROOT
from extract_crawler import Export


NAMES = {'Lines Anim', 'Heal Anim', 'Dust L', 'Dust R', 'Focus Effects', 'Focus Ring', 'White Flash R'}
REQUIRED = set(NAMES)


def extract(client=None, output=None, only=None):
    client = client or Client()
    output = output or ROOT / '_private/research/focus-effects'
    output = Path(output)
    private_actual = (ROOT / '_private' / 'research').resolve().parent
    if (not output.absolute().is_relative_to((ROOT / '_private').absolute())
            or not output.resolve().is_relative_to(private_actual)):
        raise ValueError('Focus source exports must stay under _private')
    output.mkdir(parents=True, exist_ok=True)
    candidates = [r for r in client.search('Knight') if r['type'] == 'GameObject'
                  and r['name'] == 'Knight' and r['collection'] == 'resources.assets']
    rows = []
    for row in candidates:
        go = json.loads(client.asset(row['path']))
        if go['m_TagString'] == 'Player' and go['m_Layer'] == 9:
            rows.append(row)
    if len(rows) != 1:
        raise ValueError('Expected one player tagged Knight in resources.assets')
    collection = rows[0]['path']['C']
    e = Export(client, output)
    hero = e.components(collection, rows[0]['path']['D'])
    root_transform = e.cache[hero['Transform'][0]]
    objects, seen, images = [], set(), {}
    names = set(only) if only else NAMES
    if only:
        previous = json.loads((output / 'focus-effects.json').read_text(encoding='utf-8'))
        objects = [obj for obj in previous['objects'] if obj['name'] not in names]
        images = previous['images']
        e.hashes.update(previous['source_sha256'])

    def export_image(owner_collection, ref):
        _, texture_key, texture_collection = e.resolve(owner_collection, ref)
        png_name = texture_key.removesuffix('.json') + '.png'
        if png_name not in images:
            raw = client.request('/Assets/Image',
                Path=json.dumps({'C': texture_collection, 'D': ref['m_PathID']}), Extension='png')
            (output / png_name).write_bytes(raw)
            e.hashes[png_name] = __import__('hashlib').sha256(raw).hexdigest()
            images[png_name] = dict(collection=texture_collection, path_id=ref['m_PathID'])
        return png_name

    def export_sprite_collection(owner_collection, ref):
        asset, key, collection = e.resolve(owner_collection, ref)
        structure = asset['m_Structure']
        pngs = [export_image(collection, texture) for texture in structure['textures']]
        return dict(file=key, collection=collection, images=pngs)

    def record_object(name, path, owner, go_collection):
        identity = (json.dumps(go_collection, sort_keys=True), owner)
        if identity in seen:
            return
        seen.add(identity)
        parts = e.components(go_collection, owner)
        obj = dict(name=name, path=path, owner=owner, collection=go_collection,
                   transform=parts['Transform'][0], components=parts, libraries=[],
                   particle_materials=[], images=[], sprites=[])
        for keys in parts.values():
            for key in keys:
                component = e.cache[key]
                structure = component.get('m_Structure', {})
                library_ref = structure.get('library')
                if isinstance(library_ref, dict) and library_ref.get('m_PathID'):
                    library, library_key, library_collection = e.resolve(go_collection, library_ref)
                    clips = library.get('m_Structure', {}).get('clips', [])
                    collections = {}
                    for clip in clips:
                        for frame in clip.get('frames', []):
                            ref = frame.get('spriteCollection')
                            if ref and ref.get('m_PathID'):
                                collection_id = (ref['m_FileID'], ref['m_PathID'])
                                if collection_id not in collections:
                                    collections[collection_id] = export_sprite_collection(library_collection, ref)
                    obj['libraries'].append(dict(file=library_key, clips=clips,
                                                 sprite_collections=list(collections.values())))
        for key in parts.get('ParticleSystemRenderer', []):
            renderer = e.cache[key]
            for material_ref in renderer.get('m_Materials', []):
                if not material_ref.get('m_PathID'):
                    continue
                material, material_key, material_collection = e.resolve(go_collection, material_ref)
                texture_ref = material['m_SavedProperties']['m_TexEnvs']['_MainTex']['m_Texture']
                obj['particle_materials'].append(material_key)
                obj['images'].append(export_image(material_collection, texture_ref))
        for key in parts.get('SpriteRenderer', []):
            renderer = e.cache[key]
            for material_ref in renderer.get('m_Materials', []):
                if material_ref.get('m_PathID'):
                    material, material_key, material_collection = e.resolve(go_collection, material_ref)
                    obj.setdefault('sprite_materials', []).append(material_key)
                    if material.get('m_Shader', {}).get('m_PathID'):
                        e.resolve(material_collection, material['m_Shader'])
            sprite_ref = renderer.get('m_Sprite', {})
            if not sprite_ref.get('m_PathID'):
                continue
            sprite, sprite_key, sprite_collection = e.resolve(go_collection, sprite_ref)
            texture_ref = sprite['m_RD']['m_Texture']
            obj['sprites'].append(dict(file=sprite_key, image=export_image(sprite_collection, texture_ref),
                                       rect=sprite['m_Rect'], color=renderer['m_Color']))
        objects.append(obj)

    def visit(transform_ref, parent_name=''):
        tr, _, tr_collection = e.resolve(collection, transform_ref)
        go, go_key, go_collection = e.resolve(tr_collection, tr['m_GameObject'])
        owner = tr['m_GameObject']['m_PathID']
        name = go['m_Name']
        path = f'{parent_name}/{name}' if parent_name else name
        if name in names:
            record_object(name, path, owner, go_collection)
        for child in tr.get('m_Children', []):
            visit(child, path)

    if not only:
        for child in root_transform.get('m_Children', []):
            visit(child, 'Knight')
    present = {o['name'] for o in objects}
    for name in names - present:
        candidates = [r for r in client.search(name) if r['type'] == 'GameObject'
                      and r['name'] == name and r['collection'] == 'resources.assets']
        for row in candidates:
            owner, owner_collection = row['path']['D'], row['path']['C']
            record_object(name, name, owner, owner_collection)
            parts = e.components(owner_collection, owner)
            transform = e.cache[parts['Transform'][0]]
            for child in transform.get('m_Children', []):
                visit(child, name)
    missing = REQUIRED - {o['name'] for o in objects}
    if missing:
        raise ValueError('Missing Knight Focus objects: ' + ', '.join(sorted(missing)))

    # Retain source hashes for every fetched file. The converter verifies these
    # before compiling atlas sprites or fixed-bank particle settings.
    files = sorted(e.hashes)
    manifest = dict(build=22529139, objects=objects, images=images, source_sha256=e.hashes)
    (output / 'focus-effects.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(dict(objects=[(o['owner'], o['path']) for o in objects], files=len(files))))


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--only', action='append', choices=sorted(NAMES),
                        help='Refresh named objects in an existing Focus export')
    extract(only=parser.parse_args().only)
