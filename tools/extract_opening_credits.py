"""Preserve Tutorial_01 credits animation and text from the loaded private build."""
import argparse
import hashlib
import json
from pathlib import Path

from assetripper_client import Client, ROOT
from extract_crawler import Export
from extract_tutorial_prompts import decrypt_language


def extract(client, output):
    if not output.absolute().is_relative_to(ROOT / '_private'):
        raise ValueError('Original resources must remain private')
    output.mkdir(parents=True, exist_ok=True)
    e = Export(client, output)
    row = next(r for r in client.search('tutorial_credits')
               if r['name'] == 'tutorial_credits' and r['type'] == 'GameObject')
    lc = row['path']['C']
    rc = next(r['path']['C'] for r in client.search('HeroBox') if r['collection'] == 'resources.assets')
    _, key = e.fetch(rc, 20600)
    if (output / key).read_bytes() != (ROOT / '_private/research/assets/c1-20600.json').read_bytes():
        raise ValueError('Source build differs from 22529139')
    objects, clips = {}, []

    def visit(owner):
        if str(owner) in objects:
            return
        go, key = e.fetch(lc, owner)
        parts = {'GameObject': key}
        objects[str(owner)] = parts
        for item in go['m_Components']:
            data, key, cc = e.resolve(lc, item['m_Component'])
            if 'm_LocalPosition' in data:
                parts['Transform'] = key
                for child in data['m_Children']:
                    tr, _, _ = e.resolve(cc, child)
                    visit(tr['m_GameObject']['m_PathID'])
            elif 'm_Controller' in data:
                parts['Animator'] = key
                controller, ck, c = e.resolve(cc, data['m_Controller'])
                parts['controller'] = ck
                for ref in controller['m_AnimationClips']:
                    _, clip, _ = e.resolve(c, ref)
                    clips.append(clip)
            elif 'm_Script' in data:
                script, _, _ = e.resolve(cc, data['m_Script'])
                parts[script['m_ClassName']] = key

    visit(1602)
    camera, camera_key = e.fetch(rc, 11812)
    if not camera['m_Orthographic'] or not camera['m_CullingMask']['m_Bits'] & (1 << 5):
        raise ValueError('Expected the original HUD camera for layer 5 credits')
    languages = [r for r in client.search('EN_Credits') if r['type'] == 'TextAsset']
    language = next(r for r in languages if 'List' in r['name'])
    data, key = e.fetch(language['path']['C'], language['path']['D'])
    xml = decrypt_language(data['m_Script']).encode('utf-8')
    (output / 'EN_Credits_List.xml').write_bytes(xml)
    e.hashes['EN_Credits_List.xml'] = hashlib.sha256(xml).hexdigest()
    mapping = dict(build_id=22529139, objects=objects, clips=clips, camera=camera_key, source_sha256=e.hashes)
    (output / 'credits-map.json').write_text(json.dumps(mapping, indent=2), encoding='utf-8')
    print(f'Credits: {len(objects)} owners, {len(clips)} clips, {len(e.hashes)} hashed sources')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=58291)
    parser.add_argument('--output', type=Path, default=ROOT / '_private/research/opening-credits')
    args = parser.parse_args()
    extract(Client(args.port), args.output)
