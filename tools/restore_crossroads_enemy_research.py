"""Restore enemy snapshots from build22529139 copied into AssetRipper.

Run under validation-lock.ps1 -Exclusive with the stated analysis copy loaded.
--source verifies its DLL and records disk provenance; it does not hash the
server's live serialized files. Never load or modify the Steam install.
"""
import argparse
import hashlib
import json
from pathlib import Path
import tempfile

from assetripper_client import Client, ROOT
from extract_crawler import Export, write_asset
from inspect_fsm import decode_fsm

DLL_SHA256 = 'e9048ef6a633970f735e01ec166d3959f610eaea7a88d827d48d67b1e5fb87bd'
# Stable identities verified against existing converter provenance and native GOs.
ACTORS_A = {
    'tiktik': ('level37', 221, 'Climber 1'),
    'gruzzer': ('level43', 1329, 'Fly'),
    'aspid-hatchling': ('sharedassets59.assets', 15, 'Hatcher Baby'),
}
ACTORS_B = {
    'wandering_husk': ('level37', 975, 'Zombie Runner'),
    'husk_hornhead': ('level40', 258, 'Zombie Hornhead'),
    'leaping_husk': ('level57', 924, 'Zombie Leaper'),
    'goam': ('level52', 177, 'Worm'),
}


def write_json(path, value):
    raw = (json.dumps(value, indent=2) + '\n').encode('utf-8')
    write_asset(path, raw)
    return hashlib.sha256(raw).hexdigest()


def actor_collection(client, collection, identity, name):
    rows = [row for row in client.search(name)
            if row['type'] == 'GameObject' and row['collection'] == collection
            and row['path']['D'] == identity]
    if not rows and collection.startswith('level'):
        rows = [row for row in client.search('SceneManager')
                if row['type'] == 'GameObject' and row['collection'] == collection]
    if len(rows) != 1:
        raise ValueError(f'Load {collection}:{identity} ({name}); found {len(rows)} anchors')
    return rows[0]['path']['C']


def export_art(export, collection, parts):
    libraries, collections, textures = [], {}, {}
    for filename in parts.get('tk2dSpriteAnimator', []):
        animator = export.cache[filename]['m_Structure']
        library, library_name, lc = export.resolve(collection, animator['library'])
        if library_name in libraries:
            continue
        libraries.append(library_name)
        for clip in library['m_Structure']['clips']:
            for frame in clip['frames']:
                ref = frame['spriteCollection']
                key = f"{ref['m_FileID']}:{ref['m_PathID']}"
                data, name, sc = export.resolve(lc, ref)
                if key in collections:
                    if collections[key] != name:
                        raise ValueError('Ambiguous animation collection reference')
                    continue
                collections[key] = name
                source_textures = data['m_Structure']['textures']
                if len(source_textures) != 1:
                    raise ValueError(f'Expected one texture in {name}')
                texture = source_textures[0]
                _, texture_name, tc = export.resolve(sc, texture)
                png = texture_name.removesuffix('.json') + '.png'
                if png not in export.hashes:
                    raw = export.client.request('/Assets/Image', Path=json.dumps(
                        {'C': tc, 'D': texture['m_PathID']}, separators=(',', ':')), Extension='png')
                    write_asset(export.output / png, raw)
                    export.hashes[png] = hashlib.sha256(raw).hexdigest()
                textures[name] = png
    if not libraries:
        raise ValueError('Original actor has no animation library')
    return libraries, collections, textures


def export_actor(export, spec):
    collection, identity, name = spec
    c = actor_collection(export.client, collection, identity, name)
    original, _ = export.fetch(c, identity)
    if original['m_Name'] != name:
        raise ValueError(f'Changed original actor {collection}:{identity}')
    parts = export.components(c, identity)
    libraries, collections, textures = export_art(export, c, parts)
    return c, parts, libraries, collections, textures


def source_hashes(export, source):
    collections = {name for name, _ in export.collections.values()}
    return {name: hashlib.sha256((source / name).read_bytes()).hexdigest()
            for name in sorted(collections)}


def restore(client, source, only):
    source = source.resolve()
    if hashlib.sha256((source / 'Managed/Assembly-CSharp.dll').read_bytes()).hexdigest() != DLL_SHA256:
        raise ValueError('Expected the recorded build22529139 managed assembly')
    output = ROOT / '_private/research' / ('crossroads-enemies-a' if only == 'enemy-a' else 'crossroads-enemies-b')
    scratch = ROOT / 'build'
    scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='research-enemy-', dir=scratch) as temporary:
        stage = Path(temporary)
        restore_group(client, source, only, stage)
        publish_snapshot(stage, output)


def publish_snapshot(stage, output):
    files = sorted(stage.iterdir())
    for path in files:
        target = output / path.name
        if target.exists() and target.read_bytes() != path.read_bytes():
            raise ValueError(f'Existing enemy snapshot differs: {target}')
    output.mkdir(parents=True, exist_ok=True)
    for path in files:
        target = output / path.name
        if not target.exists():
            write_asset(target, path.read_bytes())


def restore_group(client, source, only, output):
    if only == 'enemy-a':
        for stem, spec in ACTORS_A.items():
            export = Export(client, output)
            c, parts, libraries, collections, textures = export_actor(export, spec)
            corpse = {}
            death_parts = parts.get('EnemyDeathEffects', [])
            if death_parts:
                ref = export.cache[death_parts[0]]['m_Structure']['corpsePrefab']
                if ref['m_PathID']:
                    _, _, cc = export.resolve(c, ref)
                    corpse = export.components(cc, ref['m_PathID'])
            mapping = dict(build_id=22529139, actor=parts, corpse=corpse,
                           library=libraries[0], sprite_collections=collections, textures=textures,
                           source_sha256=dict(export.hashes), original_source_sha256=source_hashes(export, source))
            write_json(output / f'{stem}-map.json', mapping)
            print(f'{stem}: {len(export.hashes)} files, {len(collections)} collections', flush=True)
    else:
        export = Export(client, output)
        mapping, supplement = {}, {}
        for group, spec in ACTORS_B.items():
            c, parts, libraries, collections, textures = export_actor(export, spec)
            fsms = []
            for filename in parts.get('PlayMakerFSM', []):
                decoded = decode_fsm(export.cache[filename]['m_Structure']['fsm'])
                target = filename.removesuffix('.json') + '-fsm.json'
                export.hashes[target] = write_json(output / target, decoded)
                variables = decoded.get('variables', {}).get('floatVariables', [])
                priority = sum(v['name'] in ('Lunge Speed', 'Jump X Speed') for v in variables)
                fsms.append((priority, target))
            fsms = [name for _, name in sorted(fsms, key=lambda row: -row[0])]
            children = {}
            transform = export.cache[parts['Transform'][0]]
            for ref in transform['m_Children']:
                child, _, cc = export.resolve(c, ref)
                child_id = child['m_GameObject']['m_PathID']
                child_object, _ = export.fetch(cc, child_id)
                children[child_object['m_Name']] = export.components(cc, child_id)
            if group != 'goam':
                trigger = children.get('Attack Range', {})
                if not trigger.get('BoxCollider2D') or not trigger.get('Transform'):
                    raise ValueError(f'{group} lacks the original Attack Range trigger')
                if not fsms:
                    raise ValueError(f'{group} lacks its original control FSM')
            supplement[group] = dict(children=children)
            mapping[group] = dict(parts=parts, libraries=libraries, fsms=fsms,
                                  sprite_collections=collections, textures=textures)
        export.hashes['supplement.json'] = write_json(output / 'supplement.json', supplement)
        mapping.update(build_id=22529139, source_sha256=dict(export.hashes),
                       original_source_sha256=source_hashes(export, source))
        write_json(output / 'enemies-map.json', mapping)
        print(f'enemy-b: {len(export.hashes)} files, {len(ACTORS_B)} actors', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--only', required=True, choices=('enemy-a', 'enemy-b'))
    parser.add_argument('--port', type=int, default=58301)
    parser.add_argument('--source', type=Path, required=True, help='Copied hollow_knight_Data loaded in AssetRipper')
    args = parser.parse_args()
    restore(Client(args.port), args.source, args.only)
