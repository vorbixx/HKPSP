"""Export immediate shared plant Hit prefab hierarchies; keep unresolved links explicit.

This is a bounded dependency observation, not a particle/audio runtime conversion.
"""
import argparse
import hashlib
import json
from pathlib import Path

from assetripper_client import Client, ROOT
from extract_crawler import Export, native_component_kind
from analyze_plant_effects import verify_inputs, active_modules, require


FIELDS = ('audioSourcePrefab', 'dustHitRegularPrefab', 'dustHitDownPrefab',
          'strikeEffectPrefab', 'nailHitEffectPrefab', 'spellHitEffectPrefab')


def references(value, location=''):
    if isinstance(value, dict):
        if set(value) == {'m_FileID', 'm_PathID'}:
            if value['m_PathID']:
                yield location, value
        else:
            for key, child in value.items():
                yield from references(child, location + '/' + key)
    elif isinstance(value, list):
        for index, child in enumerate(value):
            yield from references(child, location + '/' + str(index))


def unresolved_links(cache, tables):
    result = []
    for name, deps in tables.items():
        for location, ref in references(cache[name]):
            require(str(ref['m_FileID']) in deps, 'Missing dependency table entry')
            key = f'{deps[str(ref["m_FileID"])]}-{ref["m_PathID"]}.json'
            if key not in cache:
                result.append(dict(source=name, field=location, target=key))
    return result


def extract(client, output):
    require(output.resolve().is_relative_to(ROOT / '_private'), 'Exports must remain private')
    require(not output.exists(), 'Use a fresh output directory')
    source = ROOT / '_private/research/plant-effects'
    manifest, saved = verify_inputs(source, ROOT / '_private/research/room-visuals',
                                    ROOT / '_private/research/decompiled/all')
    live = {row['path']['D']: row for row in client.search('health plant')
            if row['type'] == 'GameObject' and row['collection'] == 'level6'}
    require({p['owner'] for p in manifest['plants']} <= live.keys(), 'Missing live plant anchors')
    output.mkdir(parents=True)
    export = Export(client, output)
    objects, collections, anchors, plants = {}, {}, {}, []

    def hierarchy(collection, go_id):
        go, key = export.fetch(collection, go_id)
        if key in objects:
            return key
        require(len(objects) < 128, 'Unexpectedly large shared Hit hierarchy')
        row = dict(name=go['m_Name'], active=go['m_IsActive'], components={}, children=[])
        objects[key] = row
        collections[key] = collection
        for entry in go['m_Components']:
            component, name, target = export.resolve(collection, entry['m_Component'])
            collections[name] = target
            kind = native_component_kind(component)
            if 'm_Script' in component:
                script, _, _ = export.resolve(target, component['m_Script'])
                kind = script['m_ClassName']
            if kind is None:
                # Preserve unidentified native components rather than guessing their type.
                kind = 'Unclassified'
            row['components'].setdefault(kind, []).append(name)
            if kind == 'Transform':
                for child_ref in component['m_Children']:
                    child, _, cc = export.resolve(target, child_ref)
                    _, _, gc = export.resolve(cc, child['m_GameObject'])
                    row['children'].append(hierarchy(gc, child['m_GameObject']['m_PathID']))
        return key

    for plant in manifest['plants']:
        collection = live[plant['owner']]['path']['C']
        go, key = export.fetch(collection, plant['owner'])
        require(export.hashes[key] == manifest['root_anchors'][key], 'Changed live plant root')
        anchors[key] = export.hashes[key]
        for entry in go['m_Components']:
            _, key, _ = export.resolve(collection, entry['m_Component'])
            require(export.hashes[key] == manifest['root_anchors'][key], 'Changed live plant component')
            anchors[key] = export.hashes[key]
        data = saved[plant['breakable']]['m_Structure']
        shared = {}
        for field in FIELDS:
            transform, name, target = export.resolve(collection, data[field])
            owner, _, gc = export.resolve(target, transform['m_GameObject'])
            shared[field] = dict(reference=name, owner=hierarchy(gc, transform['m_GameObject']['m_PathID']))
        clip, clip_key, _ = export.resolve(collection, data['breakAudioEvent']['Clip'])
        plants.append(dict(owner=plant['owner'], prefabs=shared, audio=dict(
            clip=clip_key, name=clip.get('m_Name'), event=data['breakAudioEvent'],
            clip_table=data['breakAudioClipTable'])))

    tables = {}
    for name, collection in collections.items():
        _, deps = export.collection(collection)
        tables[name] = {str(index): item[1] for index, item in deps.items()}
    unresolved = unresolved_links(export.cache, tables)
    behavior_source = ROOT / '_private/research/decompiled/all'
    behavior_names = {'Breakable', 'AudioEvent', 'AudioClipTable'} | {
        kind for obj in objects.values() for kind in obj['components']
        if kind not in ('Unclassified', 'Transform', 'ParticleSystem', 'ParticleSystemRenderer', 'SpriteRenderer')}
    behaviors = {name + '.cs': hashlib.sha256((behavior_source / (name + '.cs')).read_bytes()).hexdigest()
                 for name in behavior_names if (behavior_source / (name + '.cs')).exists()}
    particles = {name: active_modules(export.cache[name]) for obj in objects.values()
                 for name in obj['components'].get('ParticleSystem', [])}
    report = dict(build=22529139, scene='Tutorial_01', plants=plants,
        objects=objects, active_particle_modules=particles, unresolved=unresolved,
        dependency_tables=tables, behavior_sha256=behaviors,
        plant_effects_manifest_sha256=hashlib.sha256((source / 'plant-effects.json').read_bytes()).hexdigest(),
        anchors=anchors, source_sha256=export.hashes,
        limitation='Immediate shared prefab topology only; unresolved links, textures, animations, audio bytes and runtime remain unaccepted.')
    (output / 'plant-hit-dependencies.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    return report


def verify_saved(output):
    report = json.loads((output / 'plant-hit-dependencies.json').read_bytes())
    source = ROOT / '_private/research/plant-effects'
    manifest, saved = verify_inputs(source, ROOT / '_private/research/room-visuals',
                                ROOT / '_private/research/decompiled/all')
    require((report['build'], report['scene']) == (22529139, 'Tutorial_01'), 'Changed build/scene')
    require(report['plant_effects_manifest_sha256'] == hashlib.sha256(
        (source / 'plant-effects.json').read_bytes()).hexdigest(), 'Changed effect manifest')
    cache = {}
    for name, expected in report['source_sha256'].items():
        require(name == Path(name).name and '\\' not in name, 'Nonlocal dependency path')
        raw = (output / name).read_bytes()
        require(hashlib.sha256(raw).hexdigest() == expected, 'Changed shared dependency: ' + name)
        cache[name] = json.loads(raw)
    for name, expected in report['anchors'].items():
        require(expected == manifest['root_anchors'][name] == report['source_sha256'][name], 'Changed anchor')
    expected_anchors = {}
    for plant in manifest['plants']:
        key = f'level6-{plant["owner"]}.json'
        for name in [key] + [f'level6-{c["m_Component"]["m_PathID"]}.json'
                             for c in saved[key]['m_Components']]:
            expected_anchors[name] = manifest['root_anchors'][name]
    require(report['anchors'] == expected_anchors, 'Incomplete live anchors')
    behavior_source = ROOT / '_private/research/decompiled/all'
    for name, expected in report['behavior_sha256'].items():
        require(name == Path(name).name and '\\' not in name, 'Nonlocal behavior path')
        require(hashlib.sha256((behavior_source / name).read_bytes()).hexdigest() == expected,
                'Changed shared behavior: ' + name)
    require(len(report['plants']) == 4 and {p['owner'] for p in report['plants']} ==
            {p['owner'] for p in manifest['plants']}, 'Changed plant owners')
    for plant in report['plants']:
        require(set(plant['prefabs']) == set(FIELDS), 'Missing shared prefab field')
        for prefab in plant['prefabs'].values():
            require(prefab['reference'] in cache and prefab['owner'] in report['objects'], 'Missing prefab')
    for name, modules in report['active_particle_modules'].items():
        require(modules == active_modules(cache[name]), 'Changed particle module profile')
    particle_names = {name for obj in report['objects'].values()
                      for name in obj['components'].get('ParticleSystem', [])}
    require(set(report['active_particle_modules']) == particle_names, 'Omitted particle system')
    require(report['unresolved'] == unresolved_links(cache, report['dependency_tables']), 'Changed unresolved links')
    for owner, obj in report['objects'].items():
        data = cache[owner]
        require(data['m_Name'] == obj['name'] and data['m_IsActive'] == obj['active'], 'Changed prefab identity')
        component_names = {name for group in obj['components'].values() for name in group}
        require(component_names == {owner.rsplit('-', 1)[0] + '-' + str(e['m_Component']['m_PathID']) + '.json'
                                    for e in data['m_Components']}, 'Omitted prefab component')
        for name in component_names:
            require(cache[name]['m_GameObject'] == dict(m_FileID=0, m_PathID=int(owner.rsplit('-', 1)[1][:-5])),
                    'Changed component owner')
        transform, = obj['components']['Transform']
        require(obj['children'] == [owner.rsplit('-', 1)[0] + '-' + str(cache[
            owner.rsplit('-', 1)[0] + '-' + str(ref['m_PathID']) + '.json']['m_GameObject']['m_PathID']) + '.json'
            for ref in cache[transform]['m_Children']], 'Changed prefab children')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=58291)
    parser.add_argument('--verify', action='store_true')
    parser.add_argument('--output', type=Path, default=ROOT / '_private/research/plant-hit-dependencies-verified')
    args = parser.parse_args()
    report = verify_saved(args.output) if args.verify else extract(Client(args.port), args.output)
    print(json.dumps(dict(plants=len(report['plants']), objects=len(report['objects']),
        particle_systems=len(report['active_particle_modules']), files=len(report['source_sha256']),
        unresolved_links=len(report['unresolved']))))


if __name__ == '__main__':
    main()
