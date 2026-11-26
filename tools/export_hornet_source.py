"""Snapshot the additive Protector scene from the immutable copied original."""
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    sys.path.insert(0, str(ROOT/'_private/tools/boss-python'))
    import UnityPy
    from watcher_unity import AlignedGenerator
    from inspect_fsm import decode_fsm
    source = ROOT/'_private/original/Hollow Knight/hollow_knight_Data'
    output = ROOT/'_private/research/hornet-protector'
    env = UnityPy.Environment(path=str(source))
    generator = AlignedGenerator('6000.0.61f1')
    generator.load_local_dll_folder(str(source/'Managed'))
    env.typetree_generator = generator
    files, hashes = {}, {}

    def load(name):
        if name not in files:
            hashes[name] = hashlib.sha256((source/name).read_bytes()).hexdigest()
            env.load_file(str(source/name))
            files[name] = next(f for f in env.files.values() if getattr(f, 'name', None) == name)
        return files[name]

    def resolve(owner, ref):
        name = owner.name if not ref['m_FileID'] else Path(owner.externals[ref['m_FileID']-1].path).name
        return load(name).objects[ref['m_PathID']]

    def export(scene, selected=None):
      rows = []
      for reader in scene.objects.values():
        if reader.type.name != 'GameObject':
            continue
        obj = reader.read_typetree()
        if selected and not any(word in obj['m_Name'].lower() for word in selected):
            continue
        row = dict(name=obj['m_Name'], path_id=reader.path_id, active=obj['m_IsActive'], components=[])
        for entry in obj['m_Component']:
            component = resolve(scene, entry['component'])
            data = component.read_typetree()
            kind = component.type.name
            if data.get('m_Script', {}).get('m_PathID'):
                kind = resolve(component.assets_file, data['m_Script']).read_typetree()['m_ClassName']
            record = dict(type=kind, path_id=component.path_id, data=data)
            if kind == 'PlayMakerFSM' and 'states' in data.get('fsm', {}):
                record['decoded'] = decode_fsm(data['fsm'])
            row['components'].append(record)
        rows.append(row)
      return rows

    rows = export(load('level133'))
    output.mkdir(parents=True, exist_ok=True)
    (output/'scene.json').write_text(json.dumps(rows, indent=2)+'\n', encoding='utf-8')
    (output/'room.json').write_text(json.dumps(export(load('level132')), indent=2)+'\n', encoding='utf-8')
    (output/'animations.json').write_text(json.dumps(load('sharedassets105.assets').objects[69].read_typetree(), indent=2)+'\n', encoding='utf-8')
    # Keep localization alongside the FSM evidence without introducing a new probe.
    resources = load('resources.assets')
    for obj in resources.objects.values():
        if obj.type.name == 'TextAsset':
            data = obj.read_typetree()
            if data.get('m_Name', '').startswith('EN_'):
                (output/(data['m_Name']+'.txt')).write_text(data['m_Script'], encoding='utf-8')
    (output/'source.json').write_text(json.dumps(dict(build_id=22529139, files=hashes), indent=2)+'\n')
    print(f'Exported {len(rows)} additive actors to {output}')


if __name__ == '__main__':
    main()
