"""Export the Tutorial_01 controller and its visited (small) title dependencies."""
import argparse
import hashlib
import json
from pathlib import Path

from assetripper_client import Client, ROOT
from extract_crawler import Export, write_asset
from extract_tutorial_prompts import decrypt_language
from inspect_fsm import decode_fsm


def extract(client=None, output=None):
    output = output or ROOT / '_private/research/area-title'
    output.mkdir(parents=True, exist_ok=True)
    client = client or Client()
    rows = client.search('Area Title')
    rc = next(r['path']['C'] for r in rows if r['name'] == 'Area Title' and r['collection'] == 'resources.assets')
    lc = next(r['path']['C'] for r in rows if r['collection'] == 'level6')
    e = Export(client, output)
    build, _ = e.fetch(rc, 20600)
    if build != json.loads((ROOT / '_private/research/hud/resources.assets-20600.json').read_text()):
        raise ValueError('Source build differs from 22529139')
    objects = {}
    for c, ids in ((lc, (743, 3682)), (rc, (7454, 7622, 7359, 7718, 7567, 4156))):
        for owner in ids:
            name, _ = e.collection(c)
            objects[f'{name}:{owner}'] = e.components(c, owner)
    fsms = {k: decode_fsm(v['m_Structure']['fsm']) for k, v in e.cache.items()
            if 'fsm' in v.get('m_Structure', {})}
    text = e.cache['resources.assets-25441.json']['m_Structure']
    font, fk, fc = e.resolve(rc, text['m_fontAsset'])
    font = font['m_Structure']
    _, ak, ac = e.resolve(fc, font['atlas'])
    png = ak.replace('.json', '.png')
    raw = client.request('/Assets/Image', Path=json.dumps({'C': ac, 'D': font['atlas']['m_PathID']}), Extension='png')
    (output / png).write_bytes(raw)
    e.hashes[png] = hashlib.sha256(raw).hexdigest()
    language, _ = e.fetch(rc, 691)
    xml = decrypt_language(language['m_Script']).encode('utf-8')
    (output / 'EN_Titles.xml').write_bytes(xml)
    e.hashes['EN_Titles.xml'] = hashlib.sha256(xml).hexdigest()
    # Preserve the title's complete UI transform chain and camera projection.
    for ident in (11486, 11184, 8904, 10734):
        e.fetch(rc, ident)
    mapping = dict(build_id=22529139, objects=objects, fsms=fsms,
                   font=fk, atlas=png, source_sha256=e.hashes)
    (output / 'title-map.json').write_text(json.dumps(mapping, indent=2), encoding='utf-8')
    print(f'Area title: {len(objects)} owners; {len(e.hashes)} hashed dependencies')


def extract_large(client, output):
    """Export the shared first-visit title, including its original fleur atlas."""
    output.mkdir(parents=True, exist_ok=True)
    row = next(r for r in client.search('Area Title')
               if r['name'] == 'Area Title' and r['collection'] == 'resources.assets')
    rc = row['path']['C']
    e = Export(client, output)
    objects = {str(owner): e.components(rc, owner)
               for owner in (7836, 7580, 7650, 7532, 7928)}
    fonts, textures, collections, libraries = {}, {}, {}, {}
    def image(c, ref):
        _, key, tc = e.resolve(c, ref)
        png = key.removesuffix('.json') + '.png'
        if png not in e.hashes:
            raw = client.request('/Assets/Image', Path=json.dumps({'C': tc, 'D': ref['m_PathID']}), Extension='png')
            write_asset(output / png, raw)
            e.hashes[png] = hashlib.sha256(raw).hexdigest()
        return png
    for parts in objects.values():
        for key in parts.get('TextMeshPro', []):
            ref = e.cache[key]['m_Structure']['m_fontAsset']
            font, fk, fc = e.resolve(rc, ref)
            fonts[fk] = image(fc, font['m_Structure']['atlas'])
        for key in parts.get('tk2dSpriteAnimator', []):
            library, lk, lc = e.resolve(rc, e.cache[key]['m_Structure']['library'])
            libraries[lk] = lk
            for clip in library['m_Structure']['clips']:
                if clip['name'] not in ('Top Appear', 'Bottom Appear'): continue
                for frame in clip['frames']:
                    coll, ck, cc = e.resolve(lc, frame['spriteCollection'])
                    if ck not in collections:
                        collections[ck] = [image(cc, t) for t in coll['m_Structure']['textures']]
        transform = e.cache[parts['Transform'][0]]
        while transform['m_Father']['m_PathID']:
            transform, _, _ = e.resolve(rc, transform['m_Father'])
    mapping = dict(build_id=22529139, objects=objects, fonts=fonts,
                   collections=collections, libraries=libraries, source_sha256=e.hashes)
    write_asset(output / 'large-title-map.json', json.dumps(mapping, indent=2).encode())
    print(f'Large title: {len(objects)} owners, {len(fonts)} fonts, {len(collections)} sprite collections, {len(e.hashes)} dependencies')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--large', action='store_true')
    parser.add_argument('--port', type=int, default=58291)
    parser.add_argument('--output', type=Path, default=ROOT / '_private/research/area-title')
    args = parser.parse_args()
    if args.large: extract_large(Client(args.port), args.output)
    else: extract()
