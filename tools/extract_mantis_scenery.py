"""Export level180's native Mantis arena scenery into the room-visual schema.

Only native GameObject/Transform/SpriteRenderer data is read. Runtime tk2d
Lords remain in the Mantis actor converter; copied original pixels stay private.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]


def serialized(value):
    """Normalize UnityPy's native vectors/rectangles to the existing export schema."""
    aliases = {k: 'm_' + k[0].upper() + k[1:] for k in
               ('x', 'y', 'z', 'w', 'r', 'g', 'b', 'a', 'width', 'height',
                'stream', 'offset', 'format', 'dimension')}
    if isinstance(value, dict):
        return {aliases.get(k, k): serialized(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [serialized(v) for v in value]
    if isinstance(value, bytes):
        return base64.b64encode(value).decode('ascii')
    return value


def extract(source, output, packages):
    if not output.resolve().is_relative_to((ROOT / '_private/research').resolve()):
        raise ValueError('Original exports must remain private')
    sys.path.insert(0, str(packages))
    import UnityPy
    from extract_crawler import write_asset
    environment = UnityPy.Environment()
    environment.path = str(source.resolve())
    environment.load_file(str(source / 'level180'))
    scene = next(f for f in environment.files.values() if getattr(f, 'name', '') == 'level180')
    output.mkdir(parents=True, exist_ok=True)
    hashes, cache, inventory, sprites, owner_components = {}, {}, [], {}, {}

    def file_for(name):
        found = next((f for f in environment.files.values() if getattr(f, 'name', '') == name), None)
        if found is None:
            environment.load_file(str(source / name))
            found = next(f for f in environment.files.values() if getattr(f, 'name', '') == name)
        return found

    def resolve(collection, reference):
        if reference['m_FileID']:
            collection = file_for(Path(collection.externals[reference['m_FileID'] - 1].path).name)
        ident = reference['m_PathID']
        return collection, ident, f'{collection.name}-{ident}.json'

    def save(name, data):
        raw = json.dumps(data, indent=2).encode('utf-8')
        write_asset(output / name, raw)
        hashes[name] = hashlib.sha256(raw).hexdigest()
        return data

    def fetch(collection, ident):
        key = f'{collection.name}-{ident}.json'
        if key not in cache:
            cache[key] = save(key, serialized(collection.objects[ident].read_typetree()))
        return cache[key], key

    def parts(ident):
        go, key = fetch(scene, ident)
        result = {}
        for component in go['m_Component']:
            cid = component['component']['m_PathID']
            kind = scene.objects[cid].type.name
            # Native components are sufficient for static visual dependencies.
            # Owner FSMs are handled by the encounter; do not deserialize MonoBehaviours.
            if kind == 'MonoBehaviour':
                continue
            _, ck = fetch(scene, cid)
            result.setdefault(kind, []).append(ck)
        owner_components[key] = result
        return result

    for ident, obj in scene.objects.items():
        if obj.type.name != 'SpriteRenderer':
            continue
        renderer, key = fetch(scene, ident)
        go, owner = fetch(scene, renderer['m_GameObject']['m_PathID'])
        components = parts(renderer['m_GameObject']['m_PathID'])
        chain, owners = [], []
        tid = int(components['Transform'][0].rsplit('-', 1)[1].split('.')[0])
        while True:
            transform, tk = fetch(scene, tid)
            chain.append(tk)
            parent_go, gk = fetch(scene, transform['m_GameObject']['m_PathID'])
            owners.append(gk)
            parts(transform['m_GameObject']['m_PathID'])
            if not transform['m_Father']['m_PathID']:
                break
            tid = transform['m_Father']['m_PathID']
        active = bool(renderer['m_Enabled']) and all(cache[k]['m_IsActive'] for k in owners)
        sc, sid, sprite_key = resolve(scene, renderer['m_Sprite'])
        if sprite_key not in sprites:
            native = sc.objects[sid].read_typetree()
            sprite = serialized(native)
            rd = sprite['m_RD']
            for old, new in (('texture', 'm_Texture'), ('textureRect', 'm_TextureRect'),
                             ('textureRectOffset', 'm_TextureRectOffset'), ('settingsRaw', 'm_SettingsRaw')):
                rd[new] = rd.pop(old)
            rd['m_IndexBuffer'] = base64.b64encode(bytes(native['m_RD']['m_IndexBuffer'])).decode('ascii')
            rd['m_VertexData']['m_Data'] = rd['m_VertexData'].pop('m_DataSize')
            save(sprite_key, sprite)
            tc, texture_id, texture_key = resolve(sc, rd['m_Texture'])
            target = texture_key.removesuffix('.json') + '.png'
            if target not in hashes:
                # Texture2D.image uses the same top-left PNG orientation as AssetRipper.
                stream = source / (tc.name + '.resS')
                if stream.exists():
                    environment.load_file(str(stream))
                tc.objects[texture_id].read().image.save(output / target)
                hashes[target] = hashlib.sha256((output / target).read_bytes()).hexdigest()
            sprites[sprite_key] = target
        materials = []
        for reference in renderer['m_Materials']:
            mc, mid, mk = resolve(scene, reference)
            material, _ = fetch(mc, mid)
            properties = material.get('m_SavedProperties', {})
            for name in ('m_Colors', 'm_Floats', 'm_TexEnvs'):
                if isinstance(properties.get(name), list):
                    properties[name] = dict(properties[name])
            save(mk, material)
            materials.append(mk)
        inventory.append(dict(renderer=key, go=owner, active=active, chain=chain,
                              owners=owners, sprite=sprite_key, materials=materials, components=components))
    # Map-zone 9's source particle prefab (resources.assets GameObject6586).
    resources = file_for('resources.assets')
    particle_rows = []
    def normalize_particle(value):
        if isinstance(value, dict):
            return {(k if k.startswith('m_') else 'm_' + k[0].upper() + k[1:]):
                    normalize_particle(v) for k, v in value.items()}
        if isinstance(value, list):
            return [normalize_particle(v) for v in value]
        return value
    def particles(owner, parents):
        go = resources.objects[owner].read_typetree()
        components = {resources.objects[c['component']['m_PathID']].type.name:
                      c['component']['m_PathID'] for c in go['m_Component']}
        transform = resources.objects[components['Transform']].read_typetree()
        chain = [transform] + parents
        if 'ParticleSystem' in components:
            material_id = resources.objects[components['ParticleSystemRenderer']].read_typetree()['m_Materials'][0]['m_PathID']
            material, _ = fetch(resources, material_id)
            properties = material['m_SavedProperties']
            for name in ('m_Colors', 'm_Floats', 'm_TexEnvs'):
                if isinstance(properties.get(name), list):
                    properties[name] = dict(properties[name])
            texture = properties['m_TexEnvs']['_MainTex']['m_Texture']
            if texture['m_PathID'] not in (415, 406):
                raise ValueError('Fungus ambient texture differs from resident mote sprite')
            particle_rows.append(dict(owner=owner, name=go['m_Name'],
                chain=normalize_particle(chain), material=material, texture=texture['m_PathID'],
                system=normalize_particle(resources.objects[components['ParticleSystem']].read_typetree()),
                renderer=normalize_particle(resources.objects[components['ParticleSystemRenderer']].read_typetree())))
        for child in transform['m_Children']:
            t = resources.objects[child['m_PathID']].read_typetree()
            particles(t['m_GameObject']['m_PathID'], chain)
    particles(6586, [])
    save('fungus-ambient.json', dict(emitters=particle_rows,
        source_sha256=hashlib.sha256((source / 'resources.assets').read_bytes()).hexdigest()))
    # The alternate original mote joins the resident decor atlas after the
    # original sprite, preserving all existing sprite indices.
    fungal = ROOT / '_private/research/ambient-particles-fungal'
    fungal.mkdir(parents=True, exist_ok=True)
    stream = source / 'resources.assets.resS'
    if stream.exists():
        environment.load_file(str(stream))
    texture = resources.objects[406]
    if texture.type.name != 'Texture2D':
        raise ValueError('Fungal mote406 is not the original Texture2D')
    png = 'resources.assets-406.png'
    texture.read().image.save(fungal / png)
    fungal_manifest = dict(build=22529139,
        media={'resources.assets-406.json': dict(png=png)},
        source_sha256={png: hashlib.sha256((fungal / png).read_bytes()).hexdigest()},
        source_resources_sha256=hashlib.sha256((source / 'resources.assets').read_bytes()).hexdigest())
    write_asset(fungal / 'ambient-particles.json', json.dumps(fungal_manifest, indent=2).encode())
    manifest = dict(build=22529139, scene='Fungus2_15_boss', inventory=inventory,
                    sprites=sprites, source_sha256=hashes, owner_components=owner_components,
                    source_level_sha256=hashlib.sha256((source / 'level180').read_bytes()).hexdigest())
    write_asset(output / 'room-visuals-map.json', json.dumps(manifest, indent=2).encode())
    print(json.dumps(dict(renderers=len(inventory), active=sum(r['active'] for r in inventory),
                          sprites=len(sprites), files=len(hashes))))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / '_private/original/Hollow Knight/hollow_knight_Data')
    parser.add_argument('--output', type=Path, default=ROOT / '_private/research/crossroads-scenes/Fungus2_15/visuals/level180')
    parser.add_argument('--packages', type=Path, default=ROOT / '_private/tools/boss-python')
    args = parser.parse_args()
    extract(args.source, args.output, args.packages)
