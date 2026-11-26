"""Export the original mask, Soul vessel and Geo HUD dependencies privately."""
import argparse
import hashlib
import json
from pathlib import Path

from assetripper_client import Client, ROOT
from extract_crawler import Export
from inspect_fsm import decode_fsm


def extract(client, output):
    output = output.absolute()
    if not output.is_relative_to(ROOT / '_private'):
        raise ValueError('Original HUD resources must remain private')
    output.mkdir(parents=True, exist_ok=True)
    root = next(r for r in client.search('HUD Cln')
                if r['name'] == 'HUD Cln' and r['collection'] == 'resources.assets')
    collection = root['path']['C']
    e = Export(client, output)
    # Bind this extraction to the already recorded, immutable source build.
    known = ROOT / '_private/research/assets/c1-20600.json'
    hero, hero_key = e.fetch(collection, 20600)
    if hashlib.sha256((output / hero_key).read_bytes()).digest() != hashlib.sha256(known.read_bytes()).digest():
        raise ValueError('Loaded resources differ from the researched source build')
    objects = {}
    for name, owner in [('mask', 4029), ('mask2', 4690), ('geo', 4066), ('frame', 4031),
                        ('soul', 5361), ('liquid', 4016), ('eyes', 5250), ('counter', 4639)]:
        objects[name] = e.components(collection, owner)
    libraries = {}
    for key in (20665, 20843):
        _, filename = e.fetch(collection, key)
        libraries[str(key)] = filename
    collections = {}
    for key in (21155, 21919):
        data, filename = e.fetch(collection, key)
        refs = data['m_Structure']['textures']
        if len(refs) != 1:
            raise ValueError('Expected a single-texture HUD collection')
        _, texture_key, texture_collection = e.resolve(collection, refs[0])
        raw = client.request('/Assets/Image', Path=json.dumps(
            {'C': texture_collection, 'D': refs[0]['m_PathID']}), Extension='png')
        png = texture_key.removesuffix('.json') + '.png'
        (output / png).write_bytes(raw)
        e.hashes[png] = hashlib.sha256(raw).hexdigest()
        collections[str(key)] = {'collection': filename, 'texture': png}
    # Geo's dynamic Unity font embeds the original OpenType bytes.
    objects['text'] = e.components(collection, 4850)
    font, font_key, _ = e.resolve(collection, {'m_FileID': 4, 'm_PathID': 76})
    if font['m_Name'] != 'TrajanPro-Bold' or not font['m_FontData']:
        raise ValueError('Unexpected Geo typeface')
    fsms = []
    for parts in objects.values():
        for filename in parts.get('PlayMakerFSM', []):
            fsms.append({'file': filename, 'fsm': decode_fsm(e.cache[filename]['m_Structure']['fsm'])})
    (output / 'fsm-decoded.json').write_text(json.dumps(fsms, indent=2), encoding='utf-8')
    mapping = dict(build_id=22529139, objects=objects, libraries=libraries, font=font_key,
                   collections=collections, source_sha256=e.hashes)
    (output / 'hud-map.json').write_text(json.dumps(mapping, indent=2), encoding='utf-8')
    print(f'HUD source: {len(objects)} owners, {len(collections)} collections, {len(e.hashes)} hashed dependencies')


def extract_layout(client, output):
    """Keep the authored relative HUD placement, independent of capture resolution."""
    output = output.absolute()
    if not output.is_relative_to(ROOT / '_private'):
        raise ValueError('Original HUD resources must remain private')
    output.mkdir(parents=True, exist_ok=True)
    root = next(r for r in client.search('HUD Cln')
                if r['name'] == 'HUD Cln' and r['collection'] == 'resources.assets')
    collection = root['path']['C']; e = Export(client, output)
    _, hero = e.fetch(collection, 20600)
    if hashlib.sha256((output/hero).read_bytes()).digest() != hashlib.sha256((ROOT/'_private/research/hud/resources.assets-20600.json').read_bytes()).digest():
        raise ValueError('HUD source build changed')
    mapping = json.loads((ROOT/'_private/research/hud/hud-map.json').read_text())
    poses = {}
    def pose(key):
        if not key:
            return (0., 0., 1., 1.)
        if key not in poses:
            t, _ = e.fetch(collection, key)
            if t['m_LocalRotation'] != dict(m_X=0, m_Y=0, m_Z=0, m_W=1):
                raise ValueError('HUD layout needs unrotated transforms')
            x,y,sx,sy = pose(t['m_Father']['m_PathID'])
            p,s = t['m_LocalPosition'],t['m_LocalScale']
            poses[key] = (x+p['m_X']*sx, y+p['m_Y']*sy, sx*s['m_X'], sy*s['m_Y'])
        return poses[key]
    owners = {name:int(mapping['objects'][name]['Transform'][0].removesuffix('.json').rsplit('-',1)[1])
              for name in ('frame','mask','mask2','geo')}
    counter, _ = e.fetch(collection, int(mapping['objects']['counter']['GeoCounter'][0].removesuffix('.json').rsplit('-',1)[1]))
    text, _ = e.fetch(collection, counter['m_Structure']['geoTextMesh']['m_PathID'])
    parts = e.components(collection, text['m_GameObject']['m_PathID'])
    owners['text'] = int(parts['Transform'][0].removesuffix('.json').rsplit('-',1)[1])
    # Pending text shares the same native TextMesh baseline and parent hierarchy.
    parts = e.components(collection, 5602)
    owners['pending'] = int(parts['Transform'][0].removesuffix('.json').rsplit('-',1)[1])
    positions = {name:pose(key) for name,key in owners.items()}
    fx,fy,fsx,fsy = positions['frame']
    relative = {name:[(x-fx)/fsx,(y-fy)/fsy,sx/fsx,sy/fsy]
                for name,(x,y,sx,sy) in positions.items()}
    details = dict(build_id=22529139, origin='HUD_frame', owners=owners,
                   text_anchor=text['m_Anchor'], font_size=text['m_FontSize'],
                   relative=relative, source_sha256=e.hashes)
    (output/'layout-map.json').write_text(json.dumps(details,indent=2))
    print('HUD authored layout:', json.dumps(relative))


def extract_liquid(client, output):
    """Export the liquid's actual shader mask and relative HUD transforms."""
    output = output.absolute()
    if not output.is_relative_to(ROOT / '_private'):
        raise ValueError('Original HUD resources must remain private')
    output.mkdir(parents=True, exist_ok=True)
    root = next(r for r in client.search('HUD Cln') if r['name']=='HUD Cln' and r['collection']=='resources.assets')
    collection = root['path']['C'];e = Export(client, output)
    _, hero = e.fetch(collection, 20600)
    if hashlib.sha256((output/hero).read_bytes()).digest()!=hashlib.sha256((ROOT/'_private/research/hud/resources.assets-20600.json').read_bytes()).digest():
        raise ValueError('HUD source build changed')
    material, _ = e.fetch(collection, 20)
    ref = material['m_SavedProperties']['m_TexEnvs']['_AlphaTex']['m_Texture']
    _, texture_key, target = e.resolve(collection, ref)
    png = texture_key.removesuffix('.json')+'.png'
    raw = client.request('/Assets/Image',Path=json.dumps({'C':target,'D':ref['m_PathID']}),Extension='png')
    (output/png).write_bytes(raw);e.hashes[png]=hashlib.sha256(raw).hexdigest()
    def transform(key):return e.fetch(collection,key)[0]
    mask, liquid, fill, eyes = [transform(key) for key in (10520,9228,10002,9949)]
    # Frame Transform is discovered from the recorded object map, not a session ID.
    mapping=json.loads((ROOT/'_private/research/hud/hud-map.json').read_text())
    frame_id=int(mapping['objects']['frame']['Transform'][0].removesuffix('.json').rsplit('-',1)[1])
    frame=transform(frame_id)
    pos=lambda t: [t['m_LocalPosition']['m_'+axis] for axis in ('X','Y')]
    center=[a+b for a,b in zip(pos(fill),pos(mask))]
    details=dict(build_id=22529139,alpha=png,
        mask_size=[mask['m_LocalScale']['m_'+a] for a in ('X','Y')],
        liquid_x=pos(liquid)[0]-pos(mask)[0],mask_y=pos(mask)[1],
        liquid_scale=liquid['m_LocalScale']['m_X'],
        frame_offset=[a-b for a,b in zip(pos(frame),center)],
        eyes_offset=[a-b for a,b in zip(pos(eyes),center)],source_sha256=e.hashes)
    (output/'liquid-map.json').write_text(json.dumps(details,indent=2))
    print('HUD liquid mask/transforms exported')


def extract_feedback(client, output):
    """Export only the Soul feedback owners and ordinary HP Up prefab."""
    output = output.absolute()
    if not output.is_relative_to(ROOT / '_private'):
        raise ValueError('Original HUD resources must remain private')
    output.mkdir(parents=True, exist_ok=True)
    root = next(r for r in client.search('HUD Cln')
                if r['name'] == 'HUD Cln' and r['collection'] == 'resources.assets')
    collection = root['path']['C']; e = Export(client, output)
    _, hero = e.fetch(collection, 20600)
    if (output / hero).read_bytes() != (ROOT / '_private/research/hud' / hero).read_bytes():
        raise ValueError('HUD feedback source build changed')
    objects, visuals, materials, textures = {}, {}, {}, {}

    def texture(c, ref):
        _, key, target = e.resolve(c, ref)
        if key not in textures:
            png = key.removesuffix('.json') + '.png'
            raw = client.request('/Assets/Image', Path=json.dumps({'C': target, 'D': ref['m_PathID']}), Extension='png')
            (output / png).write_bytes(raw)
            e.hashes[png] = hashlib.sha256(raw).hexdigest(); textures[key] = png
        return key

    def material(c, ref):
        data, key, target = e.resolve(c, ref)
        if key not in materials:
            shader, shader_key, _ = e.resolve(target, data['m_Shader'])
            entry = dict(shader=shader_key, shader_name=shader['m_Name'], textures={})
            for name, env in data['m_SavedProperties']['m_TexEnvs'].items():
                if env['m_Texture']['m_PathID']:
                    entry['textures'][name] = texture(target, env['m_Texture'])
            materials[key] = entry
        return key

    def owner(key, children=False):
        parts = e.components(collection, key); objects[str(key)] = parts
        for kind in ('SpriteRenderer', 'ParticleSystemRenderer'):
            for filename in parts.get(kind, []):
                data = e.cache[filename]
                entry = dict(materials=[material(collection, ref) for ref in data['m_Materials'] if ref['m_PathID']])
                if kind == 'SpriteRenderer':
                    sprite, sprite_key, c = e.resolve(collection, data['m_Sprite'])
                    entry.update(sprite=sprite_key, texture=texture(c, sprite['m_RD']['m_Texture']))
                visuals[filename] = entry
        if children:
            for ref in e.cache[parts['Transform'][0]]['m_Children']:
                child, _, _ = e.resolve(collection, ref)
                owner(child['m_GameObject']['m_PathID'], True)

    for key in (5361, 4690, 6389, 4449, 5166): owner(key)
    for key in (4258, 5625): owner(key, True)
    fsms = [dict(file=n, fsm=decode_fsm(e.cache[n]['m_Structure']['fsm']))
            for parts in objects.values() for n in parts.get('PlayMakerFSM', [])]
    (output / 'fsm-decoded.json').write_text(json.dumps(fsms, indent=2), encoding='utf-8')
    mapping = dict(build_id=22529139, objects=objects, visuals=visuals, materials=materials,
                   textures=textures, source_sha256=e.hashes)
    (output / 'feedback-map.json').write_text(json.dumps(mapping, indent=2), encoding='utf-8')
    print(f'HUD feedback: {len(objects)} owners, {len(textures)} textures, {len(e.hashes)} hashed dependencies')


def extract_limiter(client, output):
    """Export the Soul Limiter transition burst and limiter-up particle effect."""
    output = output.absolute()
    if not output.is_relative_to(ROOT / '_private'):
        raise ValueError('Original HUD resources must remain private')
    output.mkdir(parents=True, exist_ok=True)
    root = next(r for r in client.search('HUD Cln')
                if r['name'] == 'HUD Cln' and r['collection'] == 'resources.assets')
    collection = root['path']['C']; e = Export(client, output)
    _, hero = e.fetch(collection, 20600)
    if (output / hero).read_bytes() != (ROOT / '_private/research/hud' / hero).read_bytes():
        raise ValueError('HUD limiter source build changed')

    objects, visuals, materials, textures = {}, {}, {}, {}

    def texture(c, ref):
        _, key, target = e.resolve(c, ref)
        if key not in textures:
            png = key.removesuffix('.json') + '.png'
            raw = client.request('/Assets/Image', Path=json.dumps({'C': target, 'D': ref['m_PathID']}), Extension='png')
            (output / png).write_bytes(raw)
            e.hashes[png] = hashlib.sha256(raw).hexdigest(); textures[key] = png
        return key

    def material(c, ref):
        data, key, target = e.resolve(c, ref)
        if key not in materials:
            shader, shader_key, _ = e.resolve(target, data['m_Shader'])
            entry = dict(shader=shader_key, shader_name=shader['m_Name'], textures={})
            for name, env in data['m_SavedProperties']['m_TexEnvs'].items():
                if env['m_Texture']['m_PathID']:
                    entry['textures'][name] = texture(target, env['m_Texture'])
            materials[key] = entry
        return key

    def owner(key):
        parts = e.components(collection, key); objects[str(key)] = parts
        for kind in ('SpriteRenderer', 'ParticleSystemRenderer'):
            for filename in parts.get(kind, []):
                data = e.cache[filename]
                entry = dict(materials=[material(collection, ref) for ref in data['m_Materials'] if ref['m_PathID']])
                if kind == 'SpriteRenderer':
                    sprite, sprite_key, c = e.resolve(collection, data['m_Sprite'])
                    entry.update(sprite=sprite_key, texture=texture(c, sprite['m_RD']['m_Texture']))
                visuals[filename] = entry
        for filename in parts.get('tk2dSprite', []):
            data = e.cache[filename]['m_Structure']
            atlas, atlas_key, atlas_collection = e.resolve(collection, data['collection'])
            sprite_id = data['_spriteId']
            definition = atlas['m_Structure']['spriteDefinitions'][sprite_id]
            texture_ref = atlas['m_Structure']['textures'][0]
            texture_key = texture(atlas_collection, texture_ref)
            visuals[filename] = dict(sprite_collection=atlas_key, sprite_id=sprite_id,
                                     sprite_name=definition['name'], texture=texture_key,
                                     bounds=definition['boundsData'], uvs=definition['uvs'])

    for key in (5361, 6493, 4177):
        owner(key)

    # Include the specific shared atlas/library and every transform above both effects.
    library, library_key = e.fetch(collection, 20665)
    atlas, atlas_key = e.fetch(collection, 21155)
    texture_key = texture(collection, atlas['m_Structure']['textures'][0])
    parent_ids = set()
    for parts in objects.values():
        for filename in parts.get('Transform', []):
            transform, _ = e.fetch(collection, int(filename.removesuffix('.json').rsplit('-', 1)[1]))
            parent = transform['m_Father']['m_PathID']
            while parent and parent not in parent_ids:
                parent_ids.add(parent)
                transform, _ = e.fetch(collection, parent)
                parent = transform['m_Father']['m_PathID']
    fsms = [dict(file=n, fsm=decode_fsm(e.cache[n]['m_Structure']['fsm']))
            for parts in objects.values() for n in parts.get('PlayMakerFSM', [])]
    (output / 'fsm-decoded.json').write_text(json.dumps(fsms, indent=2), encoding='utf-8')
    mapping = dict(build_id=22529139, objects=objects, visuals=visuals, materials=materials,
                   textures=textures, library=library_key, atlas=atlas_key, atlas_texture=texture_key,
                   ancestor_transforms=sorted(parent_ids), source_sha256=e.hashes)
    (output / 'limiter-map.json').write_text(json.dumps(mapping, indent=2), encoding='utf-8')
    print(f'HUD limiter: {len(objects)} owners, {len(textures)} textures, {len(e.hashes)} hashed dependencies')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=58291)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--liquid', action='store_true')
    parser.add_argument('--layout', action='store_true')
    parser.add_argument('--feedback', action='store_true')
    parser.add_argument('--limiter', action='store_true')
    args = parser.parse_args()
    output = args.output or ROOT / '_private/research' / ('hud-limiter' if args.limiter else 'hud-feedback' if args.feedback else 'hud-layout' if args.layout else 'hud-liquid' if args.liquid else 'hud')
    (extract_limiter if args.limiter else extract_feedback if args.feedback else extract_layout if args.layout else extract_liquid if args.liquid else extract)(Client(args.port), output)
