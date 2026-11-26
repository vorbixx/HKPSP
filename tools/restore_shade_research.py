"""Rebuild the missing Shade source snapshot from verified original resources.

Use validation-lock.ps1 -Exclusive. Values come from native records; this does
not run the player or change the converter's approximations.
"""
import argparse
import hashlib
import json

from assetripper_client import Client, ROOT
from convert_shade import CLIPS
from extract_crawler import Export, write_asset
from restore_managed_research import ORIGINAL_SHA256, verify_collection


def restore(port=None, *, client=None, assembly=None, output=None):
    assembly = assembly or ROOT / '_private/converted/research-input/hollow_knight_Data/Managed/Assembly-CSharp.dll'
    if hashlib.sha256(assembly.read_bytes()).hexdigest() != ORIGINAL_SHA256:
        raise ValueError('Original assembly SHA-256 mismatch')
    client = client or Client(port)
    row = next(r for r in client.search('Hollow Shade')
               if r['collection'] == 'resources.assets' and r['path']['D'] == 4541)
    collection = row['path']['C']
    verify_collection(client, collection, 'resources.assets')
    stage = output or ROOT / '_private/converted/research-shade-stage'
    stage.mkdir(parents=True, exist_ok=True)
    export = Export(client, stage)

    def save(name, raw):
        write_asset(stage / name, raw)
        export.hashes[name] = hashlib.sha256(raw).hexdigest()

    def record(name, value):
        save(name, (json.dumps(value, indent=2) + '\n').encode())

    def asset(ident):
        return export.fetch(collection, ident)[0]

    def image(c, ident):
        _, name = export.fetch(c, ident)
        png = name.removesuffix('.json') + '.png'
        if png not in export.hashes:
            raw = client.request('/Assets/Image', Path=json.dumps({'C': c, 'D': ident}), Extension='png')
            if not raw.startswith(b'\x89PNG\r\n\x1a\n'):
                raise ValueError('Expected original texture PNG')
            save(png, raw)
        return png

    library = asset(22801)['m_Structure']
    clips = [c for c in library['clips'] if c['name'] in CLIPS]
    if {c['name'] for c in clips} != CLIPS:
        raise ValueError('Original Shade clips missing')
    collections, textures = {}, {}
    for clip in clips:
        for frame in clip['frames']:
            ref = frame['spriteCollection']
            key = f"{ref['m_FileID']}:{ref['m_PathID']}"
            if key in collections:
                continue
            data, name, owner = export.resolve(collection, ref)
            collections[key] = name
            if len(data['m_Structure']['textures']) != 1:
                raise ValueError('Expected one original Shade atlas per collection')
            tex = data['m_Structure']['textures'][0]
            _, _, tc = export.resolve(owner, tex)
            textures[name] = image(tc, tex['m_PathID'])
    facts = {f'resources.assets:{i}': asset(i) for i in (13756, 13588, 9321, 13642)}
    record('collider-presentation-facts.json', facts)

    hit_names = []
    for ident in (21014, 21298, 3613, 3023, 23335, 21091, 13511, 13543,
                  24340, 14505, 9392, 15586):
        data = asset(ident)
        hit_names.append(f'resources.assets-{ident}.json')
        if 'm_RD' in data:
            ref = data['m_RD']['m_Texture']
            _, tex_name, tc = export.resolve(collection, ref)
            hit_names.extend((tex_name, image(tc, ref['m_PathID'])))
    hit_names.extend(('resources.assets-280.json', image(collection, 280)))
    for name in sorted(set(hit_names)):
        save('hit-recovery/' + name, (stage / name).read_bytes())
    record('hit-recovery/asset-export-manifest.json',
           dict(files_sha256={name: export.hashes[name] for name in sorted(set(hit_names))}))

    def emitter(go):
        parts = export.components(collection, go)
        if len(parts.get('ParticleSystem', [])) != 1:
            raise ValueError('Expected one native Shade particle system')
        return (export.cache[parts['ParticleSystem'][0]],
                export.cache[parts['Transform'][0]])

    root = export.components(collection, 4541)
    children = []
    def walk(transform):
        for ref in transform['m_Children']:
            child, _, _ = export.resolve(collection, ref)
            go = child['m_GameObject']['m_PathID']
            parts = export.components(collection, go)
            children.append((export.cache[parts['GameObject'][0]]['m_Name'], go, parts))
            walk(child)
    walk(export.cache[root['Transform'][0]])
    idle = [(name, go) for name, go, parts in children
            if 'ParticleSystem' in parts and name == 'Shade Particles' and go == 4344]
    if len(idle) != 1:
        raise ValueError('Expected Shade Particles child4344; found ' + repr([(n, g) for n, g, p in children if 'ParticleSystem' in p]))
    emitters = []
    for go in (idle[0][1], 3975):
        native, transform = emitter(go)
        emitters.append(dict(initial=native['m_InitialModule'], emission=native['m_EmissionModule'],
                             force=native['m_ForceModule'], shape=native['m_ShapeModule'],
                             transform=transform, duration=native['m_LengthInSec']))
    record('presentation-remainder/idle-retreat-emitters.json', emitters)
    native_emitters = []
    for name, ident in (('Recovery Shade Particles', 9402), ('Particle Wave', 10972)):
        transform = asset(ident)
        save(f'recovery-children/resources.assets-{ident}.json', (stage / f'resources.assets-{ident}.json').read_bytes())
        native, _ = emitter(transform['m_GameObject']['m_PathID'])
        native_emitters.append(dict(name=name, native=native))
    record('presentation-remainder/native-emitters.json', native_emitters)
    png = image(collection, 420)
    save('presentation-remainder/' + png, (stage / png).read_bytes())
    record('presentation-remainder/source_sha256.json',
           {name.split('/', 1)[1]: digest for name, digest in export.hashes.items()
            if name.startswith('presentation-remainder/')})
    mapping = dict(build=22529139, original_sha256=ORIGINAL_SHA256,
                   libraries=dict(shade='resources.assets-22801.json'),
                   sprite_collections=collections, textures=textures, source_sha256=dict(export.hashes))
    record('shade-map.json', mapping)
    if output is not None:
        print('hollow-shade: exported', len(export.hashes), 'original/derived snapshot files')
        return
    output = ROOT / '_private/research/hollow-shade'
    for name in export.hashes:
        target = output / name
        if target.exists() and target.read_bytes() != (stage / name).read_bytes():
            raise ValueError('Refusing different existing source: ' + str(target))
    for name in export.hashes:
        write_asset(output / name, (stage / name).read_bytes())
    print('hollow-shade: restored', len(export.hashes), 'original/derived snapshot files')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=58301)
    restore(parser.parse_args().port)
