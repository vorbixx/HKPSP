"""Preserve the source opening overlay and enabled fade actions, without textures."""
import hashlib
import json
import shutil
from assetripper_client import Client, ROOT
from extract_crawler import Export


def extract():
    output = ROOT / '_private/research/opening-blanker'
    output.mkdir(parents=True, exist_ok=True)
    client = Client()
    row = next(r for r in client.search('2dtk Blanker') if r['type'] == 'GameObject')
    e = Export(client, output)
    _, guard = e.fetch(row['path']['C'], 20600)
    if (output / guard).read_bytes() != (ROOT / '_private/research/assets/c1-20600.json').read_bytes():
        raise ValueError('Source build differs from 22529139')
    _, camera = e.fetch(row['path']['C'], 20521)
    owner, _ = e.fetch(row['path']['C'], row['path']['D'])
    for ref in owner['m_Components']:
        data, key, _ = e.resolve(row['path']['C'], ref['m_Component'])
        if 'fsm' in data.get('m_Structure', {}):
            blanker = key
    shutil.copyfile(ROOT / '_private/research/decompiled/all/HeroController.cs', output / 'HeroController.cs')
    e.hashes['HeroController.cs'] = hashlib.sha256((output / 'HeroController.cs').read_bytes()).hexdigest()
    mapping = dict(build_id=22529139, camera=camera, blanker=blanker, source_sha256=e.hashes)
    (output / 'source-map.json').write_text(json.dumps(mapping, indent=2) + '\n')
    print('Opening blanker: enabled CameraFade + 2dtk Blanker actions and HeroController entry wait')


if __name__ == '__main__':
    extract()
