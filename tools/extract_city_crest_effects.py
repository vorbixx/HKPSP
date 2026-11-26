"""Targeted immutable City gate particle recovery; run under -Exclusive."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

from assetripper_client import ROOT


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rocks', action='store_true', help='Recover only the separate Bump-state Rocks emitter')
    args = parser.parse_args()
    sys.dont_write_bytecode = True
    sys.path[:0] = [str(ROOT / '_private/tools/boss-python'), str(ROOT / '_private/tools/audio-python')]
    import UnityPy
    source = ROOT / '_private/original/Hollow Knight/hollow_knight_Data'
    output = ROOT / ('_private/minimum-obstacles/city-crest-rocks' if args.rocks else
                     '_private/minimum-obstacles/city-crest-effects')
    output.mkdir(parents=True, exist_ok=True)
    files = {}

    def file(name):
        if name not in files:
            files[name] = next(iter(UnityPy.load(str(source / name)).files.values()))
        return files[name]

    def resolve(name, pointer):
        external = pointer['m_FileID']
        if external:
            name = Path(str(file(name).externals[external - 1].path)).name
        return name, pointer['m_PathID']

    recovered = set()
    dependencies = []

    def recover(name, ident):
        key = (name, ident)
        if not ident or key in recovered:
            return
        recovered.add(key)
        obj = file(name).objects[ident]
        tree = obj.read_typetree(check_read=obj.type.name != 'MonoBehaviour')
        if obj.type.name == 'MonoBehaviour':
            tree['_baseFieldsOnly'] = True
        stem = f'{name.replace(".assets", "")}-{ident}'
        (output / f'{stem}.json').write_text(json.dumps(tree, indent=2, default=lambda b: dict(byteLength=len(b), sha256=hashlib.sha256(b).hexdigest())) + '\n')
        dependencies.append(dict(file=name, id=ident, type=obj.type.name, name=tree.get('m_Name', '')))
        if obj.type.name == 'Transform':
            recover(*resolve(name, tree['m_Father']))
        elif obj.type.name in ('ParticleSystemRenderer', 'MeshRenderer'):
            for pointer in tree['m_Materials']:
                recover(*resolve(name, pointer))
        elif obj.type.name == 'Material':
            recover(*resolve(name, tree['m_Shader']))
            for _, entry in tree['m_SavedProperties']['m_TexEnvs']:
                recover(*resolve(name, entry['m_Texture']))
        elif obj.type.name == 'Texture2D':
            obj.read().image.save(output / f'{stem}.png')
        elif obj.type.name == 'MonoBehaviour':
            recover(*resolve(name, tree['m_Script']))
        elif obj.type.name == 'MeshFilter':
            recover(*resolve(name, tree['m_Mesh']))

    scene = file('level186')
    children = []
    transforms = (2372,) if args.rocks else (2160, 2684, 3329, 1907, 3088, 3389, 2660, 1813)
    for transform in transforms:
        tr = scene.objects[transform].read_typetree()
        go_id = tr['m_GameObject']['m_PathID']
        go = scene.objects[go_id].read_typetree()
        if args.rocks and (go_id != 654 or go['m_Name'] != 'Rocks'):
            raise ValueError('Changed original City Bump rocks owner')
        recover('level186', go_id)
        components = []
        for entry in go['m_Component']:
            ident = entry['component']['m_PathID']
            obj = scene.objects[ident]
            recover('level186', ident)
            components.append(dict(id=ident, type=obj.type.name))
        children.append(dict(transform=transform, game_object=go_id, name=go['m_Name'],
                             active=go['m_IsActive'], components=components))
    report = dict(source=str(source / 'level186'), children=children, dependencies=dependencies)
    (output / 'source-report.json').write_text(json.dumps(report, indent=2) + '\n')
    for item in dependencies:
        if item['type'] in ('Material', 'Texture2D', 'Shader', 'MonoScript'):
            print(item)
    print(f'Recovered {len(recovered)} targeted source objects')


if __name__ == '__main__':
    main()
