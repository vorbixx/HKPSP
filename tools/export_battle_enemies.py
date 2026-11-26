"""Save the four battle-wave actors from the copied original (build 22529139)."""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

import converted_files  # Keep copied private assets independent of hardlink seeds.
from assetripper_client import ROOT

ACTORS = [('brawler', 'level169', 'Mushroom Brawler 1'),
          ('sentry', 'level92', 'Ruins SentryB 1'),
          ('heavy', 'level92', 'Ruins Sentry FatB'),
          ('flying', 'level92', 'Ruins Flying SentryB'),
          # Mushroom Brawler Spit: FlingObjectsFromGlobalPoolVel prefab (level169 FileID 15, path 276).
          ('glob', 'sharedassets34.assets', 'Vomit Glob NoiseFix'),
          # Brooding Mawlek head and Super Spit shots (Fling ext4:197).
          ('shot', 'sharedassets34.assets', 'Shot Mawlek NoDrip')]


def export(source, output, packages):
    if not converted_files.private_output(output, ROOT):
        raise ValueError('Original enemy resources must remain private')
    sys.path.insert(0, str(packages))
    import UnityPy
    from UnityPy.helpers.TypeTreeGenerator import TypeTreeGenerator
    env = UnityPy.Environment(path=str(source))
    generator = TypeTreeGenerator('6000.0.61f1')
    generator.load_local_dll_folder(str(source / 'Managed'))
    env.typetree_generator = generator
    files, hashes = {}, {}
    output.mkdir(parents=True, exist_ok=True)

    def load(name):
        if name not in files:
            path = source / name
            hashes[name] = hashlib.sha256(path.read_bytes()).hexdigest()
            env.load_file(str(path))
            files[name] = next(f for f in env.files.values() if getattr(f, 'name', None) == name)
        return files[name]

    def resolve(owner, ref):
        if ref['m_FileID'] > len(owner.externals):
            raise ValueError(f'Bad reference in {owner.name}: {ref}; externals={len(owner.externals)}')
        name = owner.name if ref['m_FileID'] == 0 else Path(owner.externals[ref['m_FileID'] - 1].path).name
        return load(name).objects[ref['m_PathID']]

    saved = {}
    def save(obj, wrapper=False):
        name = f'{obj.assets_file.name}-{obj.path_id}.json'
        data = obj.read_typetree()
        if name not in saved:
            (output / name).write_text(json.dumps({'m_Structure': data} if wrapper else data), encoding='utf-8')
            saved[name] = True
        return name, data

    actors = {}
    for prefix, scene, prefab in ACTORS:
        asset = load(scene)
        gos = [o for o in list(asset.objects.values()) if o.type.name == 'GameObject'
               and o.read_typetree()['m_Name'] == prefab]
        if len(gos) != 1:
            raise ValueError(f'{scene}/{prefab}: expected one actor, found {len(gos)}')
        go = gos[0].read_typetree()
        parts, animator = {}, None
        for entry in go['m_Component']:
            obj = resolve(asset, entry['component'])
            kind = obj.type.name
            if kind == 'MonoBehaviour':
                # UnityPy's generated base tree omits the enabled-byte padding.
                # The player header is GameObject PPtr, enabled + 3 pad, script PPtr.
                file_id, path_id = struct.unpack_from('<iq', obj.get_raw_data(), 16)
                kind = resolve(obj.assets_file, dict(m_FileID=file_id, m_PathID=path_id)).read_typetree()['m_Name']
                if kind not in ('HealthManager', 'tk2dSpriteAnimator'):
                    continue
            elif kind not in ('Transform', 'BoxCollider2D', 'Rigidbody2D'):
                continue
            name, data = save(obj, obj.type.name == 'MonoBehaviour')
            parts.setdefault(kind, []).append(name)
            if kind == 'tk2dSpriteAnimator':
                animator = obj, data
        if not animator:
            raise ValueError(f'{prefab}: missing sprite animator')
        lib = resolve(animator[0].assets_file, animator[1]['library'])
        library_name, library = save(lib, True)
        collections, textures = {}, {}
        for clip in library['clips']:
            for frame in clip['frames']:
                ref = frame['spriteCollection']
                ref_key = f'{ref["m_FileID"]}:{ref["m_PathID"]}'
                if ref_key in collections:
                    continue
                collection = resolve(lib.assets_file, ref)
                collection_name, data = save(collection, True)
                collections[ref_key] = collection_name
                material = resolve(collection.assets_file, data['materials'][0])
                props = material.read_typetree()['m_SavedProperties']
                texture = resolve(material.assets_file, dict(props['m_TexEnvs'])['_MainTex']['m_Texture'])
                texture_name = f'{texture.assets_file.name}-{texture.path_id}.png'
                texture.read().image.save(output / texture_name)
                textures[collection_name] = texture_name
        actors[prefix] = dict(components=parts, animation_libraries=[library_name],
                              sprite_collections=collections, textures=textures)
        print(prefix, 'components', list(parts), 'clips', [c['name'] for c in library['clips'] if c['name']], flush=True)
    digest = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in output.iterdir()
              if p.is_file() and p.name != 'enemies-map.json'}
    mapping = dict(build=22529139, original_sha256=hashes, source_sha256=digest, actors=actors)
    (output / 'enemies-map.json').write_text(json.dumps(mapping, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT/'_private/original/Hollow Knight/hollow_knight_Data')
    # This checkout's own snapshot: _private/research may be a read-only shared tree.
    parser.add_argument('--output', type=Path, default=ROOT/'_private/converted/battle-enemies-source')
    parser.add_argument('--packages', type=Path, default=ROOT/'build/battle-python')
    args = parser.parse_args()
    export(args.source, args.output, args.packages)
