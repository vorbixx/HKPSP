"""Preserve the tutorial Stop Animator without changing the shared Fury bundle."""
import json
from assetripper_client import Client, ROOT
from extract_crawler import Export


def extract(client=None, output=None, source=None):
    source = source or ROOT / '_private/research/fury-chest'
    mapping = json.loads((source / 'fury-chest-map.json').read_text())
    output = output or ROOT / '_private/research/fury-reveals-source-20261023'
    output.mkdir(parents=True, exist_ok=True)
    client = client or Client()
    row = next(r for r in client.search('Charm Tutorial Msg')
               if r['collection'] == 'sharedassets6.assets' and r['path']['D'] == 491)
    export = Export(client, output)
    animator, key, collection = export.resolve(row['path']['C'], dict(m_FileID=0, m_PathID=885))
    if export.hashes[key] != mapping['source_sha256'][key]:
        raise ValueError('Loaded tutorial animator differs from the preserved source build')
    controller, controller_key, collection = export.resolve(collection, animator['m_Controller'])
    clips = []
    for reference in controller['m_AnimationClips']:
        clip, key, _ = export.resolve(collection, reference)
        clips.append(key)
        print(key, clip['m_Name'], 'float curves:', len(clip['m_FloatCurves']))
    manifest = dict(build=mapping['build'], controller=controller_key, clips=clips, source_sha256=export.hashes)
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    extract()
