"""Recreate missing Tutorial decor snapshots; run with validation-lock -Exclusive.

Raw AssetRipper JSON is preserved. The grass profile only adapts field names to
the existing converter's saved UnityPy layout; no particle values are invented.
"""
import argparse
import hashlib
import io
import json
import struct
from PIL import Image

from assetripper_client import Client, ROOT
from extract_crawler import Export, write_asset
from restore_fury_research import Snapshot
from restore_managed_research import ASSET_ANCHORS, ORIGINAL_SHA256, verify_collection


def unity_fields(value):
    if isinstance(value, list):
        return [unity_fields(v) for v in value]
    if isinstance(value, float):
        # AssetRipper writes the shortest decimal that round-trips to a Unity
        # float. UnityPy exposes that float as an exact Python double instead.
        # Restore its binary32 precision before the saved-profile arithmetic.
        return struct.unpack('<f', struct.pack('<f', value))[0]
    if not isinstance(value, dict):
        return value
    result = {}
    for key, item in value.items():
        name = key.removeprefix('m_')
        if not name.endswith('Module'):
            name = name[0].lower() + name[1:]
        result[name] = unity_fields(item)
    return result


def snapshot(client, group):
    result = Snapshot(client, group)
    result.stage = ROOT / '_private/converted/research-decor-stage' / group
    result.stage.mkdir(parents=True, exist_ok=True)
    result.export = Export(client, result.stage)
    result.mapping['asset_anchors'] = {name: dict(path_id=ident, sha256=sha)
                                      for name, (ident, sha) in ASSET_ANCHORS.items()}
    return result


def components(s, collection, ident):
    parts = s.export.components(collection, ident)
    go = s.export.cache[parts['GameObject'][0]]
    known = {key for keys in parts.values() for key in keys}
    for item in go['m_Components']:
        data, key, _ = s.export.resolve(collection, item['m_Component'])
        if key in known:
            continue
        if 'm_OutputAudioMixerGroup' in data:
            kind = 'AudioSource'
        elif 'm_StaticBatchInfo' in data:
            kind = 'MeshRenderer'
        elif 'm_Center' in data and 'm_Size' in data:
            kind = 'BoxCollider'
        else:
            continue
        parts.setdefault(kind, []).append(key)
    return parts


def ancestors(s, collection, transform):
    while transform['m_Father']['m_PathID']:
        transform, _, collection = s.export.resolve(collection, transform['m_Father'])
        s.export.resolve(collection, transform['m_GameObject'])


def grass(client, resource, scene):
    group = 'grass-effects-source'
    s = snapshot(client, group)
    parts = components(s, scene, 2874)
    cut = s.export.cache[parts['GrassCut'][0]]['m_Structure']
    particle, particle_key, owner = s.export.resolve(scene, cut['particles'])
    ident = cut['particles']['m_PathID'] if 'm_Components' in particle else particle['m_GameObject']['m_PathID']
    particle_parts = components(s, owner, ident)
    system_key = particle_parts['ParticleSystem'][0]
    raw = s.export.cache[system_key]
    renderer = s.export.cache[particle_parts['ParticleSystemRenderer'][0]]
    transform = s.export.cache[particle_parts['Transform'][0]]
    profile = unity_fields(raw)
    for name in ('radius', 'arc'):
        profile['ShapeModule'][name] = profile['ShapeModule'].pop(name + 'Parameter')
    s.texture(resource, 290)
    png = 'resources.assets-290.png'
    encoded = io.BytesIO()
    image = Image.open(s.stage / png).convert('RGBA')
    image.save(encoded, format='PNG')
    canonical = encoded.getvalue()
    # Pin the decoded pixels, not the PNG bytes: the zlib build differs between
    # platforms (the Windows re-encode was baf43e24...).
    if image.size != (46, 138) or hashlib.sha256(image.tobytes()).hexdigest() != \
            'f9846af320cc09556d7346e27e55e3a56734c5d8e86dd165098f088fdde8a579':
        raise ValueError('Grass pixels differ from pinned original PNG')
    write_asset(s.stage / png, canonical)
    s.export.hashes[png] = hashlib.sha256(canonical).hexdigest()
    source = ROOT / '_private/converted/research-input/hollow_knight_Data/resources.assets'
    sha = hashlib.sha256(source.read_bytes()).hexdigest()
    if sha != '6d49ac5a50a282a645acc32518b28e7a29d0b94a0c3b5a4cda18dade3e6fe511':
        raise ValueError('Grass resources differ from pinned original')
    s.mapping.update(schema=1, scene='Tutorial_01', owner=2874, system=profile,
                     system_key=system_key, renderer=renderer,
                     transform={label: unity_fields(transform[key]) for label, key in
                                [('local_position', 'm_LocalPosition'), ('local_scale', 'm_LocalScale'),
                                 ('local_rotation', 'm_LocalRotation')]},
                     provenance=dict(source_assets_sha256={'resources.assets': sha},
                                     adapter='AssetRipper field names to saved UnityPy layout',
                                     particle_system=system_key))
    s.publish(group, 'grass-2874-profile.json')


def door(client, resource, shared):
    group = 'great-door-vfx-source'
    s = snapshot(client, group)
    objects = {}
    materials, media = {}, {}
    for ident in (4328, 5223, 486, 500, 465):
        collection = shared if ident in (486, 500, 465) else resource
        parts = components(s, collection, ident)
        objects[str(ident)] = dict(parts=parts)
        for key in parts.get('ParticleSystemRenderer', []):
            renderer = s.export.cache[key]
            for ref in renderer['m_Materials']:
                if not ref['m_PathID']:
                    continue
                material, name, owner = s.export.resolve(collection, ref)
                env = material['m_SavedProperties']['m_TexEnvs']['_MainTex']['m_Texture']
                texture = s.texture(owner, env['m_PathID'])
                materials[name] = {'_MainTex': texture}
                media[texture] = {'png': s.mapping['visual_assets'][texture]['png']}
    rock, rock_key = s.export.fetch(resource, 22286)
    definitions = rock['m_Structure']['spriteDefinitions']
    ids = {name: next(i for i, d in enumerate(definitions) if d['name'] == name)
           for name in ('rock_01', 'rock_02', 'rock_03', 'rock_04')}
    s.texture(resource, 328)
    s.mapping.update(build_id=22529139, objects=objects,
                     collections={'rock': dict(file=rock_key, sprite_ids=ids)},
                     material_media=materials, media=media, sha256=dict(s.export.hashes))
    s.publish(group, 'vfx-source-map.json')


def contact(client, resource, scene, shared):
    group = 'platform-effects-source'
    s = snapshot(client, group)
    owners, drips, platforms = {}, [], []
    # Existing manifest identities are lookup seeds only. Every component and
    # value is fetched again from the independently verified original scene.
    seeds = [('WaterDrip', drips, (462, 690, 860, 1263, 1568, 2552, 2691, 3246, 3343)),
             ('DustyPlatform', platforms, (48, 70, 596, 760, 945, 1491, 1648, 1683, 1810,
                                          1990, 2112, 2202, 2274, 2614, 2675, 3167, 3178, 3416))]
    for kind, rows, identifiers in seeds:
        for ident in identifiers:
            parts = components(s, scene, ident)
            if kind not in parts:
                raise ValueError('Original owner lacks ' + kind + ': ' + str(ident))
            transform = s.export.cache[parts['Transform'][0]]
            ancestors(s, scene, transform)
            records = [dict(type=label, snapshot='assets/' + key.replace('.assets-', '-'),
                            sha256=s.export.hashes[key])
                       for label, keys in parts.items() for key in keys]
            owners['level6:' + str(ident)] = dict(files=records)
            result = dict(go=ident)
            if kind == 'DustyPlatform':
                keys = parts.get('BoxCollider2D', parts.get('BoxCollider', []))
                result['colliders'] = [dict(path_id=int(key.rsplit('-', 1)[1][:-5]),
                                             size=[s.export.cache[key]['m_Size']['m_' + a] for a in 'XY'])
                                        for key in keys]
                if len(result['colliders']) != 1:
                    raise ValueError('Expected one DustyPlatform box')
            rows.append(result)
    if (len(drips), len(platforms)) != (9, 18):
        raise ValueError('Incomplete original contact owners: ' + str((len(drips), len(platforms))))
    for ident in (5214, 8116, 5215):
        parts = components(s, resource, ident)
        ancestors(s, resource, s.export.cache[parts['Transform'][0]])
        for keys in parts.values():
            for key in keys:
                s.visual(resource, int(key.rsplit('-', 1)[1][:-5]))
    for ident in (1095, 1111, 1061, 1058):
        s.export.fetch(shared, ident)
    s.texture(shared, 55)
    s.export.fetch(resource, 13920)
    for value in list(s.export.cache.values()):
        if 'm_Shader' in value:
            s.export.resolve(resource, value['m_Shader'])
    files = dict(s.export.hashes)
    normalized = {}
    for name, sha in files.items():
        target = ('media/asset-ripper/' if name.endswith('.png') else 'assets/') + name.replace('.assets-', '-')
        write_asset(s.stage / target, (s.stage / name).read_bytes())
        normalized[target] = sha
    s.export.hashes = normalized
    s.mapping.update(source_build_id=22529139, scene='Tutorial_01', owners=owners,
                     water_drips=drips, dusty_platforms=platforms,
                     asset_exports=[dict(file=name, sha256=sha) for name, sha in normalized.items()])
    s.publish(group, 'manifest.json')


def restore(port, group):
    assembly = ROOT / '_private/converted/research-input/hollow_knight_Data/Managed/Assembly-CSharp.dll'
    if hashlib.sha256(assembly.read_bytes()).hexdigest() != ORIGINAL_SHA256:
        raise ValueError('Original managed assembly mismatch')
    client = Client(port)
    collections = []
    for term, name, ident in [('Hollow Shade', 'resources.assets', 4541),
                               ('Shiny Item (1)', 'level6', 2549),
                               ('Charm Tutorial Msg', 'sharedassets6.assets', 491)]:
        owner = next(r['path']['C'] for r in client.search(term)
                     if r['collection'] == name and r['path']['D'] == ident)
        verify_collection(client, owner, name)
        collections.append(owner)
    resource, scene, shared = collections
    if group == 'grass':
        grass(client, resource, scene)
    elif group == 'door':
        door(client, resource, shared)
    else:
        contact(client, resource, scene, shared)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=58301)
    parser.add_argument('--only', choices=('grass', 'door', 'contact'), required=True)
    args = parser.parse_args()
    restore(args.port, args.only)
