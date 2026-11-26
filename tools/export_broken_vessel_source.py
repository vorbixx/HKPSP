"""Export Broken Vessel evidence from the immutable copied original, never Steam."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT/'_private/original/Hollow Knight/hollow_knight_Data')
    parser.add_argument('--output', type=Path, default=ROOT/'_private/minimum-bosses/broken-vessel')
    parser.add_argument('--packages', type=Path, default=ROOT/'_private/tools/boss-python')
    args = parser.parse_args()
    source, output = args.source.resolve(), args.output.resolve()
    if not output.is_relative_to((ROOT/'_private/minimum-bosses/broken-vessel').resolve()):
        parser.error('Output must stay in the owned Broken Vessel research directory')
    # Unresolved: _private/original may link to the read-only Steam folder.
    if not Path(os.path.abspath(args.source)).is_relative_to(Path(os.path.abspath(ROOT/'_private/original'))):
        parser.error('Read a copied original under _private/original, never the Steam install')
    sys.path.insert(0, str(args.packages))
    import UnityPy
    from watcher_unity import AlignedGenerator
    from inspect_fsm import decode_fsm

    env = UnityPy.Environment(path=str(source))
    generator = AlignedGenerator('6000.0.61f1')
    generator.load_local_dll_folder(str(source/'Managed'))
    env.typetree_generator = generator
    files, hashes = {}, {}

    def load(name):
        if name not in files:
            path = source/name
            hashes[name] = hashlib.sha256(path.read_bytes()).hexdigest()
            env.load_file(str(path))
            files[name] = next(f for f in env.files.values() if getattr(f, 'name', None) == name)
        return files[name]

    def resolve(owner, reference):
        name = owner.name if reference['m_FileID'] == 0 else Path(owner.externals[reference['m_FileID']-1].path).name
        return load(name).objects[reference['m_PathID']]

    settings = next(o.read_typetree() for o in load('globalgamemanagers').objects.values()
                    if o.type.name == 'BuildSettings')
    index = next(i for i, path in enumerate(settings['scenes'])
                 if Path(path).stem == 'Abyss_19')
    scene_name = f'level{index}'
    scene = load(scene_name)
    objects = {o.path_id: o.read_typetree() for o in scene.objects.values()
               if o.type.name == 'GameObject'}
    transforms = {o.path_id: o.read_typetree() for o in scene.objects.values()
                  if o.type.name == 'Transform'}
    selected = set()

    def select(path_id):
        if path_id in selected:
            return
        selected.add(path_id)
        for entry in objects[path_id]['m_Component']:
            component = resolve(scene, entry['component'])
            if component.type.name == 'Transform':
                for child in component.read_typetree()['m_Children']:
                    transform = resolve(component.assets_file, child)
                    select(transform.read_typetree()['m_GameObject']['m_PathID'])

    terms = ('infected knight', 'infected balloon', 'battle', 'boss', 'door', 'corpse', 'scuttler spawner')
    for path_id, obj in objects.items():
        if any(term in obj['m_Name'].lower() for term in terms):
            select(path_id)
    def export_actor(reader):
        obj = reader.read_typetree()
        row = dict(name=obj['m_Name'], path_id=reader.path_id, source_file=reader.assets_file.name, active=obj['m_IsActive'],
                   layer=obj['m_Layer'], components=[])
        for entry in obj['m_Component']:
            component = resolve(reader.assets_file, entry['component'])
            data = component.read_typetree()
            kind = component.type.name
            if data.get('m_Script', {}).get('m_PathID'):
                kind = resolve(component.assets_file, data['m_Script']).read_typetree().get('m_ClassName', kind)
            record = dict(type=kind, path_id=component.path_id, data=data)
            structure = data.get('m_Structure', data)
            if 'fsm' in structure:
                if 'states' in structure['fsm']:
                    record['decoded'] = decode_fsm(structure['fsm'])
                else:
                    record['decode_error'] = 'No serialized states; template/empty FSM unresolved'
            if kind == 'tk2dSpriteAnimator':
                library = structure.get('library')
                if library and library['m_PathID']:
                    record['animation_library'] = resolve(component.assets_file, library).read_typetree()
            row['components'].append(record)
        return row

    rows = [export_actor(scene.objects[path_id]) for path_id in sorted(selected)]
    prefabs, visited = [], set()

    def export_prefab(reader):
        key = (reader.assets_file.name, reader.path_id)
        if key in visited:
            return
        visited.add(key)
        prefabs.append(export_actor(reader))
        for entry in reader.read_typetree()['m_Component']:
            component = resolve(reader.assets_file, entry['component'])
            if component.type.name == 'Transform':
                for child in component.read_typetree()['m_Children']:
                    transform = resolve(component.assets_file, child)
                    export_prefab(resolve(transform.assets_file, transform.read_typetree()['m_GameObject']))

    for row in rows:
        if row['name'] != 'Infected Knight':
            continue
        for component in row['components']:
            data = component['data']
            if component['type'] == 'EnemyDeathEffects':
                export_prefab(resolve(scene, data['corpsePrefab']))
            if component['type'] == 'PersonalObjectPool':
                for item in data['startupPool']:
                    export_prefab(resolve(scene, item['prefab']))
    for path in sorted((source/'Managed').glob('*.dll')):
        hashes[str(path.relative_to(source))] = hashlib.sha256(path.read_bytes()).hexdigest()
    manifest = ROOT/'_private/original/appmanifest_367520.acf'
    build = int(re.search(r'"buildid"\s+"(\d+)"', manifest.read_text()).group(1))
    result = dict(build_id=build, room='Abyss_19', scene=scene_name,
                  source_sha256=hashes, actors=rows, prefabs=prefabs, transforms=transforms)
    output.mkdir(parents=True, exist_ok=True)
    (output/f'{scene_name}-source.json').write_text(json.dumps(result, indent=2))
    print(json.dumps(dict(scene=scene_name, actors=len(rows), prefabs=len(prefabs), source_files=len(hashes))))


if __name__ == '__main__':
    main()
