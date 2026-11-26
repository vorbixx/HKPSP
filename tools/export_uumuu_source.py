"""Export copied-original Uumuu/Quirrel FSM evidence without modifying source assets."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scene', choices=('level223', 'level228', 'level229', 'level402', 'level403'), default='level229')
    parser.add_argument('--prefab', choices=('zap', 'corpse'),
                        help='export the referenced level229 pooled prefab and its children')
    parser.add_argument('--source', type=Path, default=ROOT/'_private/original/Hollow Knight/hollow_knight_Data')
    parser.add_argument('--output', type=Path, default=ROOT/'_private/minimum-bosses/uumuu')
    parser.add_argument('--packages', type=Path, default=ROOT/'_private/tools/boss-python')
    args = parser.parse_args()
    output = args.output.resolve()
    if not output.is_relative_to((ROOT/'_private/minimum-bosses').resolve()):
        parser.error('Output must stay in this task research directory')
    sys.path.insert(0, str(args.packages))
    import UnityPy
    from watcher_unity import AlignedGenerator
    from inspect_fsm import decode_fsm

    source = args.source.resolve()
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

    for path in sorted((source/'Managed').glob('*.dll')):
        hashes[str(path.relative_to(source))] = hashlib.sha256(path.read_bytes()).hexdigest()
    scene = load(args.scene)
    reference = None
    if args.prefab:
        if args.scene != 'level229':
            parser.error('Pooled prefabs belong to level229')
        path_id = {'zap': 28, 'corpse': 32}[args.prefab]
        reference = f'level229 external3 path{path_id}'
        scene = load(Path(scene.externals[2].path).name)
    objects = {o.path_id: o.read_typetree() for o in scene.objects.values() if o.type.name == 'GameObject'}
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

    terms = ('mega jelly', 'quirrel', 'battle', 'boss', 'door', 'dreamer', 'monomon',
             'dream dialogue', 'dream enter', 'dream impact', 'dream wake',
             'cage', 'white flash', 'cinematic', 'inspect', 'tank')
    if args.prefab:
        select(path_id)
    elif args.scene == 'level223':
        # Fungus3_47: only the one-way Archive wall (GameObject 106) and its children.
        select(106)
    else:
        for path_id, obj in objects.items():
            if args.scene == 'level403' or any(term in obj['m_Name'].lower() for term in terms):
                select(path_id)
    rows = []
    # A prefab export lists its root first: consumers read actors[0] as the
    # prefab object (the corpse FSM, the zap body).
    root = [path_id] if args.prefab else []
    for path_id in root + sorted(selected - set(root)):
        obj = objects[path_id]
        row = dict(name=obj['m_Name'], path_id=path_id, active=obj['m_IsActive'],
                   layer=obj['m_Layer'], tag=obj['m_Tag'], components=[])
        for entry in obj['m_Component']:
            component = resolve(scene, entry['component'])
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
            if kind == 'CinematicPlayer' and structure.get('videoClip', {}).get('m_PathID'):
                # CinematicPlayer references CinematicVideoReference, not VideoClip.
                video_reference = resolve(component.assets_file, structure['videoClip'])
                record['video_reference'] = video_reference.read_typetree()
                clip = resolve(video_reference.assets_file, record['video_reference']['embeddedVideoClip'])
                record['video_clip'] = clip.read_typetree()
                record['video_asset'] = clip.assets_file.name
                record['video_path_id'] = clip.path_id
                resource = record['video_clip']['m_ExternalResources']['m_Source']
                if Path(resource).name != resource:
                    raise ValueError('Cinematic resource must be a copied-source filename')
                hashes[resource] = hashlib.sha256((source/resource).read_bytes()).hexdigest()
            row['components'].append(record)
        rows.append(row)
    build = int(re.search(r'"buildid"\s+"(\d+)"', (ROOT/'_private/original/appmanifest_367520.acf').read_text()).group(1))
    output.mkdir(parents=True, exist_ok=True)
    result = dict(build_id=build, scene=args.scene, source_sha256=hashes, actors=rows)
    stem = args.scene
    if reference:
        result['source_reference'] = reference
        stem = {'zap': 'pooled-zap', 'corpse': 'corpse'}[args.prefab]
    (output/f'{stem}-source.json').write_text(json.dumps(result, indent=2))
    (output/f'{stem}-objects.json').write_text(json.dumps(objects, indent=2))
    print(json.dumps(dict(scene=args.scene, actors=len(rows), source_files=len(hashes))))


if __name__ == '__main__':
    main()
