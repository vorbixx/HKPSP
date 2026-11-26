"""Export False Knight particle instances and textures from the copied source build."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import converted_files
from assetripper_client import ROOT


def extract(args):
    sys.path.insert(0, str(args.packages))
    import UnityPy
    source, output = args.source, args.output
    if not converted_files.private_output(output, ROOT):
        raise ValueError('Source resources must remain private')
    output.mkdir(parents=True, exist_ok=True)
    env = UnityPy.load(str(source / 'level48'))
    asset = next(iter(env.files.values()))

    def resolve(owner, ref):
        if not ref['m_PathID']:
            return None
        target = owner
        if ref['m_FileID']:
            name = Path(owner.externals[ref['m_FileID'] - 1].path).name
            target = next((f for f in env.files.values() if getattr(f, 'name', '') == name), None)
            if target is None:
                env.load_file(str(source / name))
                target = next(f for f in env.files.values() if getattr(f, 'name', '') == name)
        return target.objects[ref['m_PathID']]

    env.load_file(str(source / 'resources.assets'))
    resources = next(f for f in env.files.values() if getattr(f, 'name', '') == 'resources.assets')
    records = {}
    objects = [asset.objects[i] for i in range(175, 207)]
    # Blood, its collision splash sub-emitter, and the infected death puff.
    objects += [resources.objects[i] for i in (14766, 15092, 14776, 15096, 14655, 14824)]
    for obj in objects:
        ident, owner = obj.path_id, obj.assets_file
        data = obj.read_typetree()
        record = records[str(ident)] = dict(type=obj.type.name, data=data)
        go = resolve(owner, data['m_GameObject']).read_typetree()
        record['name'] = go['m_Name']
        transforms = [resolve(owner, x['component']) for x in go['m_Component']]
        transform = next(x for x in transforms if x.type.name == 'Transform')
        record['transform'] = transform.read_typetree()
        if owner == resources:
            record['transform']['m_LocalPosition'] = dict(x=0., y=0., z=0.)
        if obj.type.name == 'ParticleSystemRenderer':
            material = resolve(owner, data['m_Materials'][0])
            mat = material.read_typetree()
            record['material'] = mat
            tex = dict(mat['m_SavedProperties']['m_TexEnvs'])['_MainTex']['m_Texture']
            texture = resolve(material.assets_file, tex)
            name = f'{texture.assets_file.name}-{texture.path_id}.png'
            texture.read().image.save(output / name)
            record['texture'] = name
    (output / 'components.json').write_text(json.dumps(records, indent=2), encoding='utf-8')
    manifest = dict(build_id=22529139, source_sha256=hashlib.sha256((source/'level48').read_bytes()).hexdigest())
    (output/'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    print(json.dumps({k: v['name'] for k, v in records.items() if v['type'] == 'ParticleSystem'}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT/'_private/original/Hollow Knight/hollow_knight_Data')
    parser.add_argument('--packages', type=Path, default=ROOT/'build/false-knight-python')
    parser.add_argument('--output', type=Path, default=ROOT/'_private/research/false-knight-effects')
    extract(parser.parse_args())
