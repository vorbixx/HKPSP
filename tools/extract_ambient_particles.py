"""Export the camera-follow ambient particles selected by Tutorial_01.

Only the selected prefab hierarchy and its particle artwork are traversed.
Original bytes remain private; --verify replays the traversal without a server.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys

from assetripper_client import Client, ROOT
from extract_decor_effects import Store, require


OUTPUT = ROOT / '_private/research/ambient-particles'
MANIFEST = 'ambient-particles.json'


def extract_mesh(source, output, packages=None):
    """Decode the built-in mesh from the readonly original copy, never Steam."""
    from convert_ambient_particles import MESH_SOURCE_SHA256, mesh_edges
    if hashlib.sha256(source.read_bytes()).hexdigest() != MESH_SOURCE_SHA256:
        raise ValueError('Changed built-in Unity resources copy')
    if packages:
        sys.path.insert(0, str(packages))
    import UnityPy
    from UnityPy.helpers.MeshHelper import MeshHandler
    objects = [obj for obj in UnityPy.load(str(source)).objects if obj.path_id == 10209]
    require(len(objects) == 1 and objects[0].type.name == 'Mesh', 'Missing Plane10209')
    handler = MeshHandler(objects[0].read())
    handler.process()
    vertices = [list(map(float, vertex)) for vertex in handler.m_Vertices]
    indices = list(map(int, handler.m_IndexBuffer))
    require(len(vertices) == 121 and len(indices) == 600, 'Changed Plane topology')
    mesh = dict(pathId=10209, sourceSha256=MESH_SOURCE_SHA256, vertices=vertices,
                triangles=[indices[i:i+3] for i in range(0, len(indices), 3)])
    mesh_edges(mesh) # Validate the recovered geometry hash before replacing the cache.
    output.mkdir(parents=True, exist_ok=True)
    (output/'mesh-10209.json').write_text(json.dumps(mesh, indent=2)+'\n')
    print('Plane10209 verified: 121 vertices, 200 triangles, 320 unique edges')


def observe(store, zone_override=None):
    # Stable asset IDs from the verified readonly Steam build 22529139.
    scene_key = 'level6-12820.json'
    scene = store.get(scene_key)['m_Structure']
    require(not scene['noParticles'], 'Tutorial particles disabled')
    camera_key = 'resources.assets-21664.json'
    camera = store.get(camera_key)['m_Structure']
    controller_key, controller = store.resolve(camera_key, camera['sceneParticlesPrefab'])
    controller = controller['m_Structure']
    zone = zone_override if zone_override is not None else scene['overrideParticlesWith'] or scene['mapZone']
    matches = [row for row in controller['sceneParticles'] if row['mapZone'] == zone]
    require(len(matches) <= 1, 'Ambiguous particle zone')
    selected = matches[0] if matches else controller['defaultParticles']
    root_key, _ = store.resolve(controller_key, selected['particleObject'])
    emitters = []
    visited = set()

    def visit(key, parents):
        require(key not in visited and len(visited) < 32, 'Invalid particle hierarchy')
        visited.add(key)
        obj = store.get(key)
        components = [store.resolve(key, row['m_Component']) for row in obj['m_Components']]
        transforms = [(k, a) for k, a in components if 'm_LocalPosition' in a]
        require(len(transforms) == 1, 'Particle object requires one Transform')
        transform_key, transform = transforms[0]
        chain = [transform_key] + parents
        systems = [(k, a) for k, a in components if 'm_InitialModule' in a]
        renderers = [(k, a) for k, a in components if 'm_SortMode' in a and 'm_Materials' in a]
        for system_key, system in systems:
            require(len(renderers) == 1, 'Particle renderer missing')
            renderer_key, renderer = renderers[0]
            require(len(renderer['m_Materials']) == 1, 'Unsupported particle material count')
            material_key, material = store.resolve(renderer_key, renderer['m_Materials'][0])
            shader_key, _ = store.resolve(material_key, material['m_Shader'])
            texture_key, _ = store.resolve(material_key, material['m_SavedProperties']['m_TexEnvs']['_MainTex']['m_Texture'])
            store.binary(texture_key, 'texture')
            mesh_ref = system['m_ShapeModule']['m_Mesh']
            mesh_key = store.resolve(system_key, mesh_ref)[0] if mesh_ref['m_PathID'] else None
            emitters.append(dict(name=obj['m_Name'], object=key, system=system_key,
                                 renderer=renderer_key, transforms=chain,
                                 material=material_key, shader=shader_key,
                                 texture=texture_key, mesh=mesh_key))
        for component_key, component in components:
            if 'm_Script' in component:
                store.resolve(component_key, component['m_Script'])
        # AssetRipper serializes children as m_Children, including inactive roots.
        for child in transform['m_Children']:
            child_key, child_transform = store.resolve(transform_key, child)
            child_object_key, _ = store.resolve(child_key, child_transform['m_GameObject'])
            visit(child_object_key, chain)

    visit(root_key, [])
    require(0 < len(emitters) <= 8, 'Unsupported ambient emitter count')
    capacities = [store.get(row['system'])['m_InitialModule']['m_MaxNumParticles'] for row in emitters]
    if zone_override is None:
        require(len(emitters) == 4, 'Tutorial default particle hierarchy changed')
        require(sum(capacities) == 184, 'Tutorial ambient capacity changed')
    return dict(build_id=22529139, scene_manager=scene_key, camera=camera_key,
                controller=controller_key, selected_zone=zone,
                used_default=not matches, root=root_key, emitters=emitters,
                capacities=capacities, total_capacity=sum(capacities))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=OUTPUT)
    parser.add_argument('--verify', action='store_true')
    parser.add_argument('--zone', type=int, help='Export the camera particle prefab for this source MapZone')
    parser.add_argument('--mesh-only', action='store_true')
    parser.add_argument('--mesh-source', type=Path, default=ROOT / '_private/original/Hollow Knight/hollow_knight_Data/Resources/unity default resources')
    parser.add_argument('--mesh-packages', type=Path, help='Optional directory containing UnityPy 1.25.2')
    args = parser.parse_args()
    if args.mesh_only:
        extract_mesh(args.mesh_source, args.output, args.mesh_packages)
        return
    if args.verify:
        report = json.loads((args.output / MANIFEST).read_text(encoding='utf-8'))
        store = Store(args.output, report=report)
        require(observe(store, report.get('zone_override')) == report['derived'], 'Changed ambient derivation')
        print('Ambient source verified: emitter configuration and artwork hashes intact')
    else:
        args.output.mkdir(parents=True, exist_ok=True)
        store = Store(args.output, Client())
        derived = observe(store, args.zone)
        report = dict(derived=derived, source_sha256=store.hashes,
                      dependency_tables=store.tables, media=store.media)
        if args.zone is not None:
            report['zone_override'] = args.zone
        (args.output / MANIFEST).write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
        print(json.dumps(derived, ensure_ascii=False))


if __name__ == '__main__':
    main()
