"""Validate saved plant effect dependencies and retain every enabled module privately."""
import hashlib
import json
from pathlib import Path

from assetripper_client import ROOT
from extract_crawler import native_component_kind


def require(ok, message):
    if not ok:
        raise ValueError(message)


def active_modules(particle):
    modules = {}
    for key, value in particle.items():
        if not key.endswith('Module'):
            continue
        require(isinstance(value, dict) and isinstance(value.get('m_Enabled'), bool),
                'Unknown particle module schema: ' + key)
        if value['m_Enabled']:
            modules[key] = value
    require('m_InitialModule' in modules, 'Missing enabled initial module')
    return modules


def verify_inputs(source, room_source, behavior_source):
    report = json.loads((source / 'plant-effects.json').read_bytes())
    require((report['build'], report['scene']) == (22529139, 'Tutorial_01'), 'Unexpected plant effects build/scene')
    room_raw = (room_source / 'room-visuals-map.json').read_bytes()
    require(hashlib.sha256(room_raw).hexdigest() == report['room_manifest_sha256'], 'Changed room manifest')
    room = json.loads(room_raw)
    cache = {}
    for name, expected in report['source_sha256'].items():
        require(name == Path(name).name and '\\' not in name and name not in ('.', '..'), 'Nonlocal effect input')
        raw = (source / name).read_bytes()
        require(hashlib.sha256(raw).hexdigest() == expected, 'Changed plant effect dependency: ' + name)
        if name.endswith('.json'):
            cache[name] = json.loads(raw)
    for name, expected in report['root_anchors'].items():
        require(name in cache and expected == report['source_sha256'][name] == room['source_sha256'][name]
                and hashlib.sha256((room_source / name).read_bytes()).hexdigest() == expected,
                'Changed root/remnant anchor: ' + name)
    for name, expected in report['behavior_sha256'].items():
        require(name == Path(name).name and '\\' not in name, 'Nonlocal behavior input')
        require(hashlib.sha256((behavior_source / name).read_bytes()).hexdigest() == expected,
                'Changed effect behavior: ' + name)
    return report, cache


def analyze(source, room_source, behavior_source):
    report, cache = verify_inputs(source, room_source, behavior_source)
    require(len(report['plants']) == 4 and len({p['owner'] for p in report['plants']}) == 4,
            'Expected four unique plant roots')
    observations = []
    for owner, obj in report['objects'].items():
        require(not obj['children'], 'Particle children need separate activation research')
        components = obj['components']
        require(set(components) == {'Transform', 'ParticleSystem', 'ParticleSystemRenderer', 'SetZRandom'}
                and all(len(v) == 1 for v in components.values()), 'Changed debris component topology')
        require(cache[owner]['m_IsActive'] == obj['active'] is False, 'Debris must start inactive')
        data = {}
        for kind, names in components.items():
            component = cache[names[0]]
            require(component['m_GameObject'] == dict(m_FileID=0, m_PathID=int(owner[7:-5])),
                    'Changed debris component owner')
            if kind != 'SetZRandom':
                require(native_component_kind(component) == kind, 'Misclassified debris component')
            data[kind] = component
        particle = data['ParticleSystem']
        observations.append(dict(owner=owner, active_modules=active_modules(particle),
            settings={k: v for k, v in particle.items() if not k.endswith('Module')},
            transform=data['Transform'], renderer=data['ParticleSystemRenderer'],
            random_z=data['SetZRandom']['m_Structure']))
    debris = [key for p in report['plants'] for key in p['debris']]
    require(len(debris) == 8 and len(set(debris)) == 8 and set(debris) == set(report['objects']),
            'Debris ownership mismatch')
    for plant in report['plants']:
        root = f"level6-{plant['owner']}.json"
        require(root in report['root_anchors'] and plant['breakable'] in report['root_anchors'],
                'Unanchored plant root')
        config = cache[plant['breakable']]['m_Structure']
        expected = [f"level6-{ref['m_PathID']}.json" for ref in config['debrisParts']
                    if ref['m_FileID'] == 0]
        require(plant['debris'] == expected and len(config['debrisParts']) == 2, 'Changed debris references')
        require(len(plant['remnants']) == 1 and all(k in report['root_anchors'] for k in plant['remnants']),
                'Unanchored remnant')
    for material, references in report['materials'].items():
        require(material in cache and references['shader'] in cache, 'Unresolved material/shader')
        for texture in references['textures'].values():
            require(texture in cache and report['textures'][texture] in report['source_sha256'],
                    'Unresolved material texture')
    return dict(build=report['build'], scene=report['scene'], observations=observations,
                source_sha256=report['source_sha256'], behavior_sha256=report['behavior_sha256'],
                materials=report['materials'], textures=report['textures'],
                runtime_unresolved=['Particle time/seed and initialization order after activation.',
                    'Curves, random distributions, module ordering and collision response.',
                    'Material blend and matched graphical samples.',
                    'Shared dust/strike/nail/spell prefabs, audio and camera shake.'])


def main():
    source = ROOT / '_private/research/plant-effects'
    report = analyze(source, ROOT / '_private/research/room-visuals', ROOT / '_private/research/decompiled/all')
    (source / 'profile.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(dict(objects=len(report['observations']), materials=len(report['materials']),
                         textures=len(report['textures']), runtime_verified=False)))


if __name__ == '__main__':
    main()
