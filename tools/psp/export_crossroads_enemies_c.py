"""Export the Crossroads group C enemies (Husk Bully, Baldur, Aspid Mother and
hatchlings, Brooding Mawlek) from a loaded AssetRipper session.

This is the exporter that produced the prepared `research/crossroads-enemies-c`
snapshot (formerly an ad hoc `export.py` inside that folder). Load levels 40,
45, 50, 57, 59 and 65 with their dependencies first.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from assetripper_client import Client
from extract_crawler import Export
from inspect_fsm import decode_fsm

PICKS = (
    ('husk_bully_candidate', 'Zombie Barger', 'level40', 899),
    ('baldur_candidate', 'Roller', 'sharedassets50.assets', 55),
    ('baldur_candidate_alt', 'Roller', 'sharedassets50.assets', 56),
    ('aspid_mother_candidate', 'Hatcher', 'sharedassets57.assets', 23),
    ('aspid_mother_candidate_alt', 'Hatcher', 'sharedassets57.assets', 25),
    ('hatcher_baby', 'Hatcher Baby', 'sharedassets59.assets', 15),
    ('hatcher_actor', 'Hatcher', 'level65', 46),
    ('brooding_mawlek', 'Mawlek Body', 'level45', 169),
)
LEVELS = (40, 45, 50, 57, 59, 65)


def export(client, out, port):
    out.mkdir(parents=True, exist_ok=True)
    e = Export(client, out)
    report = {'source_port': port, 'source_build': 22529139, 'actors': {}, 'requested_name_search': {}}

    def save_texture(collection, texref, textures, key):
        _, tk, tc = e.resolve(collection, texref)
        png = tk.removesuffix('.json') + '.png'
        if png not in textures.values():
            raw = client.request('/Assets/Image', Path=json.dumps({'C': tc, 'D': texref['m_PathID']}), Extension='png')
            (out / png).write_bytes(raw)
            e.hashes[png] = hashlib.sha256(raw).hexdigest()
        textures[key] = png

    for slug, name, scene, pid in PICKS:
        row = next(r for r in client.search(name) if r['name'] == name and r['collection'] == scene
                   and r['type'] == 'GameObject' and r['path']['D'] == pid)
        col = row['path']['C']
        parts = e.components(col, pid)
        animlibs, collections, textures, fsms, corpse = [], {}, {}, [], None
        for file in parts.get('PlayMakerFSM', []):
            data = json.loads((out / file).read_text())
            fsm = decode_fsm(data['m_Structure']['fsm'])
            fn = slug + '-' + Path(file).stem + '-fsm.json'
            (out / fn).write_text(json.dumps(fsm, indent=2))
            fsms.append(fn)
        for file in parts.get('tk2dSpriteAnimator', []):
            anim = json.loads((out / file).read_text())['m_Structure']
            lib, lk, lc = e.resolve(col, anim['library'])
            animlibs.append(lk)
            for clip in lib['m_Structure']['clips']:
                for frame in clip['frames']:
                    ref = frame['spriteCollection']
                    rk = f"{ref['m_FileID']}:{ref['m_PathID']}"
                    if rk in collections:
                        continue
                    coll, ck, sc = e.resolve(lc, ref)
                    collections[rk] = ck
                    for texref in coll['m_Structure']['textures']:
                        save_texture(sc, texref, textures, ck)
        for file in parts.get('tk2dSpriteAnimation', []):
            animlibs.append(file)
            lib = json.loads((out / file).read_text())
            for clip in lib.get('m_Structure', {}).get('clips', []):
                for frame in clip.get('frames', []):
                    ref = frame.get('spriteCollection', {})
                    rk = f"{ref.get('m_FileID')}:{ref.get('m_PathID')}"
                    if not ref.get('m_PathID') or rk in collections:
                        continue
                    coll, ck, sc = e.resolve(col, ref)
                    collections[rk] = ck
                    for texref in coll['m_Structure'].get('textures', []):
                        save_texture(sc, texref, textures, ck)
        for file in parts.get('tk2dSpriteCollectionData', []):
            coll = json.loads((out / file).read_text())
            for texref in coll.get('m_Structure', {}).get('textures', []):
                save_texture(col, texref, textures, file)
        for file in parts.get('EnemyDeathEffects', []):
            death = json.loads((out / file).read_text())['m_Structure']
            ref = death.get('corpsePrefab', {})
            if ref.get('m_PathID'):
                _, ck, cc = e.resolve(col, ref)
                corpse = {'asset': ck, 'components': e.components(cc, ref['m_PathID'])}
        report['actors'][slug] = {'source': row, 'components': parts, 'animation_libraries': animlibs,
                                  'sprite_collections': collections, 'textures': textures,
                                  'fsms': fsms, 'corpse': corpse}
        print(slug, list(parts), animlibs, len(textures), flush=True)
    report['source_sha256'] = e.hashes
    (out / 'enemies-map.json').write_text(json.dumps(report, indent=2), encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--port', type=int, default=58293)
    parser.add_argument('--output', type=Path, default=ROOT / '_private/research/crossroads-enemies-c')
    args = parser.parse_args()
    export(Client(args.port), args.output, args.port)


if __name__ == '__main__':
    main()
