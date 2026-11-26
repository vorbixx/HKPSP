"""Export only the four verified Tutorial plant debris hierarchies and materials.

Serialized activation is an observation, not a claim about runtime particles.
Original JSON stays private. All root anchors must match the saved room export.
"""
import argparse
import hashlib
import json

from assetripper_client import Client, ROOT
from extract_crawler import Export, native_component_kind


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def extract(client, source, output):
    if not output.resolve().is_relative_to((ROOT / '_private/research').resolve()):
        raise ValueError('Plant effect exports must remain private')
    raw = (source / 'room-visuals-map.json').read_bytes()
    manifest = json.loads(raw)
    if (manifest['build'], manifest['scene']) != (22529139, 'Tutorial_01'):
        raise ValueError('Unexpected plant build/scene')
    roots = {int(r['go'][7:-5]): r for r in manifest['inventory']
             if r['active'] and 'Breakable' in r['components']
             and 'PlayFromRandomFrameMecanim' in r['components']}
    if len(roots) != 4:
        raise ValueError('Expected four plant roots')
    live = {r['path']['D']: r for r in client.search('health plant')
            if r['type'] == 'GameObject' and r['collection'] == 'level6'}
    if not roots.keys() <= live.keys():
        raise ValueError('Missing live plant anchors')
    output.mkdir(parents=True, exist_ok=True)
    export = Export(client, output)
    plants, objects, materials, anchors, textures = [], {}, {}, {}, {}
    behaviors = {}
    for name, expected in {
            'Breakable.cs': '36c7acbd5a52d4ad75eac839a15235b617e3ee41ed50b534425aac91fee6779a',
            'SetZRandom.cs': '530b955ab824ae07302d33465f517a70e77c111e583aead28af5fb59ab065d56'}.items():
        actual = digest((ROOT / '_private/research/decompiled/all' / name).read_bytes())
        if actual != expected:
            raise ValueError('Changed plant effect behavior: ' + name)
        behaviors[name] = actual

    def component_group(collection, owner):
        go, key = export.fetch(collection, owner)
        if key in objects:
            return key
        if len(objects) >= 32:
            raise ValueError('Unexpectedly large plant debris hierarchy')
        row = dict(active=go['m_IsActive'], components={}, children=[])
        objects[key] = row
        for entry in go['m_Components']:
            data, name, target = export.resolve(collection, entry['m_Component'])
            kind = native_component_kind(data)
            if 'm_Script' in data:
                script, _, _ = export.resolve(target, data['m_Script'])
                kind = script['m_ClassName']
            row['components'].setdefault(kind or 'Unclassified', []).append(name)
            if kind == 'Transform':
                for child_ref in data['m_Children']:
                    child, _, cc = export.resolve(target, child_ref)
                    _, _, gc = export.resolve(cc, child['m_GameObject'])
                    row['children'].append(component_group(gc, child['m_GameObject']['m_PathID']))
            for ref in data.get('m_Materials', []):
                if not ref['m_PathID']:
                    continue
                material, mk, mc = export.resolve(target, ref)
                if mk in materials:
                    continue
                _, shader_key, _ = export.resolve(mc, material['m_Shader'])
                material_textures = {}
                for prop, env in material['m_SavedProperties']['m_TexEnvs'].items():
                    if not env['m_Texture']['m_PathID']:
                        continue
                    _, tk, tc = export.resolve(mc, env['m_Texture'])
                    material_textures[prop] = tk
                    if tk not in textures:
                        image_name = tk.removesuffix('.json') + '.png'
                        raw_image = client.request('/Assets/Image',
                            Path=json.dumps(dict(C=tc, D=env['m_Texture']['m_PathID'])), Extension='png')
                        (output / image_name).write_bytes(raw_image)
                        export.hashes[image_name] = digest(raw_image)
                        textures[tk] = image_name
                materials[mk] = dict(name=material.get('m_Name'), shader=shader_key,
                                     textures=material_textures)
        return key

    for owner, row in roots.items():
        collection = live[owner]['path']['C']
        go, _ = export.fetch(collection, owner)
        for ref in [dict(m_FileID=0, m_PathID=owner)] + [
                e['m_Component'] for e in go['m_Components']]:
            _, key, _ = export.resolve(collection, ref)
            saved = (source / key).read_bytes()
            if digest(saved) != manifest['source_sha256'][key] or export.hashes[key] != digest(saved):
                raise ValueError('Live plant root changed: ' + key)
            anchors[key] = digest(saved)
        breakable_key, = row['components']['Breakable']
        data = export.cache[breakable_key]['m_Structure']
        remnants = []
        for ref in data['remnantParts']:
            remnant, key, target = export.resolve(collection, ref)
            remnants.append(key)
            for anchor_ref in [ref] + [e['m_Component'] for e in remnant['m_Components']]:
                _, anchor, _ = export.resolve(target, anchor_ref)
                saved = (source / anchor).read_bytes()
                if digest(saved) != manifest['source_sha256'][anchor] or export.hashes[anchor] != digest(saved):
                    raise ValueError('Live plant remnant changed: ' + anchor)
                anchors[anchor] = digest(saved)
        debris = []
        for ref in data['debrisParts']:
            _, key, target = export.resolve(collection, ref)
            debris.append(component_group(target, ref['m_PathID']))
        plants.append(dict(owner=owner, breakable=breakable_key, remnants=remnants, debris=debris))
    report = dict(build=manifest['build'], scene=manifest['scene'], plants=plants,
                  objects=objects, materials=materials, textures=textures,
                  behavior_sha256=behaviors, root_anchors=anchors,
                  room_manifest_sha256=digest(raw), source_sha256=export.hashes,
                  limitation='Root anchors match saved build export; full session provenance and runtime effects are not established.')
    (output / 'plant-effects.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=58291)
    args = parser.parse_args()
    report = extract(Client(args.port), ROOT / '_private/research/room-visuals',
                     ROOT / '_private/research/plant-effects')
    print(json.dumps(dict(plants=len(report['plants']), objects=len(report['objects']),
                          materials=len(report['materials']), files=len(report['source_sha256']))))


if __name__ == '__main__':
    main()
