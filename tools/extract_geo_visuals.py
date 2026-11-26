"""Export Tutorial_01 Geo and Health Cocoon animation dependencies (private data)."""
import converted_files  # Install hardlink copy-on-write before asset writes.
import argparse
import hashlib
import json
import shutil
from pathlib import Path
from assetripper_client import Client, ROOT
from extract_crawler import Export

OWNERS = (1566, 1632, 2357, 2800, 3422)
COIN_CLIPS = ('Small Air', 'Small Idle')
ROCK_CLIPS = ('Idle 1', 'Idle 2', 'Gleam 1', 'Gleam 2', 'Broken 1', 'Broken 2')
COCOON_CLIPS = ('Cocoon Idle', 'Cocoon Sweat')
GLOB_CLIPS = ('Spawn', 'Glob Land', 'Glob Wobble')


def extract_chips(source, output=None):
    """Reuse the already exported prefab closure and identical stalactite atlas."""
    output = output or ROOT/'_private/converted/geo-chip-source'
    if not output.absolute().is_relative_to(ROOT/'_private/converted'):
        raise ValueError('Stone chip source copy must remain in private converted data')
    output.mkdir(parents=True, exist_ok=True)
    collection = 'resources.assets-22286.json'
    texture = 'resources.assets-328.png'
    known = ROOT/'_private/research/stalactite-sources'
    if (known/collection).read_bytes() != (source/collection).read_bytes():
        raise ValueError('Stone chip collection provenance differs')
    shutil.copyfile(known/texture, output/texture)
    parts = dict(debris='resources.assets-26892.json', body='resources.assets-13529.json',
                 collider='resources.assets-13697.json', bounce='resources.assets-22804.json',
                 finishing='resources.assets-27230.json')
    files = [collection, texture, 'resources.assets-4328.json', *parts.values()]
    for name in files:
        if name != texture:
            shutil.copyfile(source/name, output/name)
    mapping = dict(prefab=4328, collection=collection, texture=texture, parts=parts,
                   source_sha256={name:hashlib.sha256((output/name).read_bytes()).hexdigest() for name in files})
    (output/'geo-chips-map.json').write_text(json.dumps(mapping, indent=2), encoding='utf-8')


def extract(client, output):
    if not output.absolute().is_relative_to(ROOT/'_private'):
        raise ValueError('Original resources must remain private')
    output.mkdir(parents=True, exist_ok=True)
    e = Export(client, output)
    initial_hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in output.iterdir() if p.is_file()}
    coin = next(r for r in client.search('Geo Small')
                if r['type'] == 'GameObject' and r['collection'] == 'resources.assets')
    # The fling prefab is not a renderer child of a rock. Export its explicit
    # component/collection closure so a fresh output has the chip source files.
    chip_parts = e.components(coin['path']['C'], 4328)
    chip_sprite = e.cache[chip_parts['tk2dSprite'][0]]['m_Structure']
    e.resolve(coin['path']['C'], chip_sprite['collection'])
    rocks = {r['path']['D']: r for r in client.search('Geo Rock')
             if r['type'] == 'GameObject' and r['collection'] == 'level6'}
    cocoons = [r for r in client.search('Health Cocoon')
               if r['type'] == 'GameObject' and r['name'] == 'Health Cocoon' and r['collection'] == 'level6']
    splats = [r for r in client.search('Splat Effect')
              if r['type'] == 'GameObject' and r['name'] == 'Splat Effect' and r['collection'] == 'level6']
    globs = [r for r in client.search('Blue Glob')
             if r['type'] == 'GameObject' and r['name'] == 'Blue Glob' and r['collection'] == 'sharedassets6.assets']
    caps = [r for r in client.search('Cocoon Cap')
            if r['type'] == 'GameObject' and r['name'] == 'Cocoon Cap' and r['collection'] == 'level6']
    spatters = [r for r in client.search('Spatter Blue')
                if r['type'] == 'GameObject' and r['name'] == 'Spatter Blue' and r['collection'] == 'resources.assets']
    if len(cocoons) != 1 or len(splats) != 1 or len(globs) != 1 or len(caps) != 1 or len(spatters) != 1:
        raise ValueError('Load Tutorial_01 and its shared assets; expected one of each cocoon presentation prefab')
    groups = []
    selections = [(coin, COIN_CLIPS, 'geo', 'coin'),
                  *((rocks[i], ROCK_CLIPS, 'tutorial', 'rock') for i in OWNERS),
                  (cocoons[0], COCOON_CLIPS, 'room-visuals', 'cocoon')]
    # Splat Effect is a child of Health Cocoon. Include its default animation
    # so activation on break uses the source artwork as well.
    selections.append((splats[0], None, 'room-visuals', 'splat'))
    selections.append((globs[0], GLOB_CLIPS, 'geo-visuals', 'glob'))
    selections.append((caps[0], None, 'room-visuals', 'cap'))
    selections.append((spatters[0], None, 'geo-visuals', 'spatter'))
    for row, names, known_folder, kind in selections:
        collection = row['path']['C']
        parts = e.components(collection, row['path']['D'])
        # Bind the live session to the already researched private build.
        known_source = ROOT/'_private/research'/known_folder
        for files in parts.values():
            for name in files:
                known = known_source/name
                if not known.exists():
                    expected = initial_hashes.get(name)
                    if expected is None:
                        if kind not in ('glob', 'cap', 'spatter'):
                            raise ValueError('Missing build provenance: '+str(known))
                        continue
                else:
                    expected = hashlib.sha256(known.read_bytes()).hexdigest()
                if expected != e.hashes[name]:
                    raise ValueError('Loaded object differs from researched build: '+name)
        ancestors = []
        transform = e.cache[parts['Transform'][0]]
        tc = collection
        while transform['m_Father']['m_PathID']:
            transform, name, tc = e.resolve(tc, transform['m_Father'])
            ancestors.append(name)
        if kind in ('cap', 'spatter'):
            sprite_refs = ([ref for ref in e.cache[parts['SpatterOrange'][0]]['m_Structure']['sprites']]
                           if kind == 'spatter' else
                           [e.cache[parts['SpriteRenderer'][0]]['m_Sprite']])
            native_frames = []
            for ref in sprite_refs:
                sprite, sprite_key, sc = e.resolve(collection, ref)
                texture_ref = sprite['m_RD']['m_Texture']
                _, texture_key, texture_collection = e.resolve(sc, texture_ref)
                png = texture_key.removesuffix('.json') + '.png'
                raw = client.request('/Assets/Image', Path=json.dumps({'C': texture_collection,
                                                                      'D': texture_ref['m_PathID']}), Extension='png')
                (output/png).write_bytes(raw)
                e.hashes[png] = hashlib.sha256(raw).hexdigest()
                native_frames.append(dict(sprite=sprite_key, texture=png))
            groups.append(dict(owner=row['path']['D'], name=row['name'], kind=kind, parts=parts,
                               ancestors=ancestors, native_frames=native_frames,
                               native_sprite=native_frames[0]['sprite'], native_texture=native_frames[0]['texture']))
            continue
        animator = e.cache[parts['tk2dSpriteAnimator'][0]]['m_Structure']
        library, library_key, lc = e.resolve(collection, animator['library'])
        available = {c['name']: c for c in library['m_Structure']['clips']}
        if names is None:
            default = animator['defaultClipId']
            if not 0 <= default < len(library['m_Structure']['clips']):
                raise ValueError('Splat Effect default clip is outside its library')
            names = (library['m_Structure']['clips'][default]['name'],)
        if not set(names) <= available.keys():
            raise ValueError(f'Clip selection {names}; available {list(available)}')
        collections = {}
        textures = {}
        for name in names:
            for frame in available[name]['frames']:
                ref = frame['spriteCollection']
                refkey = f'{ref["m_FileID"]}:{ref["m_PathID"]}'
                if refkey in collections:
                    continue
                coll, ck, cc = e.resolve(lc, ref)
                collections[refkey] = ck
                if len(coll['m_Structure']['textures']) != 1:
                    raise ValueError('Expected single-material Geo collection')
                texture = coll['m_Structure']['textures'][0]
                _, tk, tc = e.resolve(cc, texture)
                png = tk.removesuffix('.json') + '.png'
                if png not in e.hashes:
                    raw = client.request('/Assets/Image', Path=json.dumps({'C': tc, 'D': texture['m_PathID']}), Extension='png')
                    (output/png).write_bytes(raw)
                    e.hashes[png] = hashlib.sha256(raw).hexdigest()
                textures[ck] = png
        groups.append(dict(owner=row['path']['D'], name=row['name'], kind=kind, parts=parts,
                           ancestors=ancestors, library=library_key, clips=names,
                           sprite_collections=collections, textures=textures))
    mapping = dict(build_id=22529139, scene='Tutorial_01', groups=groups, source_sha256=e.hashes)
    (output/'geo-visuals-map.json').write_text(json.dumps(mapping, indent=2), encoding='utf-8')
    extract_chips(output)
    print(json.dumps(dict(groups=len(groups), source_files=len(e.hashes),
                          libraries=sorted({g['library'] for g in groups if 'library' in g}))))


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, default=ROOT/'_private/research/geo-visuals')
    p.add_argument('--chips-only', action='store_true', help='Reuse existing researched prefab and atlas exports')
    p.add_argument('--port', type=int, default=58291)
    a = p.parse_args()
    if a.chips_only:
        extract_chips(a.output)
    else:
        extract(Client(a.port), a.output)
