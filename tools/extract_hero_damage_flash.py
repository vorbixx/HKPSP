"""Export the ordinary red-mask damage crack and its authored presentation data."""
import hashlib
import json
import argparse
from assetripper_client import Client, ROOT
from extract_crawler import Export
from inspect_fsm import decode_fsm


def extract_flash_animation(client, collection, output):
    """Export the native Animator skipped by Export.components' admission list."""
    e = Export(client, output)
    go, _ = e.fetch(collection, 8400)
    animators = [e.resolve(collection, c['m_Component']) for c in go['m_Components']]
    animator, ak, ac = next(item for item in animators if 'm_Controller' in item[0])
    controller, ck, cc = e.resolve(ac, animator['m_Controller'])
    if len(controller['m_AnimationClips']) != 1:
        raise ValueError('Expected one Hit Crack animation')
    clip, lk, _ = e.resolve(cc, controller['m_AnimationClips'][0])
    if clip['m_Name'] != 'hit_crack_simple':
        raise ValueError('Unexpected Hit Crack animation')
    path = output / 'damage-flash.json'
    manifest = json.loads(path.read_text())
    manifest['animation'] = dict(animator=ak, controller=ck, clip=lk)
    manifest['source_sha256'].update(e.hashes)
    path.write_text(json.dumps(manifest, indent=2)+'\n')
    print(json.dumps(dict(flash_animation=clip['m_Name'], files=len(e.hashes))))


def extract_flings(client, collection, dispatch, output=None):
    """Follow only the two black prefab dependencies used by ordinary Gen."""
    output = output or (ROOT / '_private/research/hero-damage-flings').resolve()
    output.mkdir(parents=True, exist_ok=True)
    e = Export(client, output)
    raw = (json.dumps(dispatch, indent=2)+'\n').encode()
    (output / 'dispatch.json').write_bytes(raw)
    e.hashes['dispatch.json'] = hashlib.sha256(raw).hexdigest()
    actions = [a['parameters'] for s in dispatch['states'] if s['name'] == 'Gen'
               for a in s['actions'] if a['enabled'] and a['type'].endswith('.FlingObjectsFromGlobalPool')]
    nodes = []
    for owner in dict.fromkeys(a['gameObject']['value']['m_PathID'] for a in actions):
        parts = e.components(collection, owner)
        animator = e.cache[parts['tk2dSpriteAnimator'][0]]['m_Structure']
        library, lk, lc = e.resolve(collection, animator['library'])
        clip = library['m_Structure']['clips'][animator['defaultClipId']]
        images = {}
        for frame in clip['frames']:
            sprites, sk, sc = e.resolve(lc, frame['spriteCollection'])
            texture = sprites['m_Structure']['textures'][0]
            _, tk, tc = e.resolve(sc, texture)
            png = tk.removesuffix('.json') + '.png'
            if sk not in images:
                raw = client.request('/Assets/Image', Path=json.dumps({'C': tc, 'D': texture['m_PathID']}), Extension='png')
                (output / png).write_bytes(raw)
                e.hashes[png] = hashlib.sha256(raw).hexdigest()
                images[sk] = png
        renderers = []
        for component in e.cache[parts['GameObject'][0]]['m_Components']:
            data, key, _ = e.resolve(collection, component['m_Component'])
            if 'm_SortingOrder' in data:
                renderers.append(key)
        nodes.append(dict(owner=owner, components=parts, library=lk, clip=clip,
                          images=images, renderers=renderers))
    mapping = dict(build=22529139, actions=actions, nodes=nodes, source_sha256=e.hashes)
    (output / 'damage-flings.json').write_text(json.dumps(mapping, indent=2)+'\n')
    print(json.dumps(dict(flings=[n['owner'] for n in nodes], files=len(e.hashes))))


def damage_collection(client):
    row = next(r for r in client.search('Damage Effect') if r['type'] == 'GameObject'
               and r['name'] == 'Damage Effect' and r['collection'] == 'resources.assets')
    return row['path']['C']


def extract(flings_only=False, flash_animation_only=False, *, client=None, output=None, include_flings=True):
    client = client or Client()
    output = output or ROOT / '_private/research/hero-damage-flash'
    output.mkdir(parents=True, exist_ok=True)
    collection = damage_collection(client)
    if flash_animation_only:
        extract_flash_animation(client, collection, output)
        return
    if flings_only:
        dispatch = json.loads((output / 'dispatch.json').read_text())
        extract_flings(client, collection, dispatch)
        return
    e = Export(client, output)
    root = e.components(collection, 7724)
    parts = e.components(collection, 8400)
    renderer = e.cache[parts['SpriteRenderer'][0]]
    sprite, sk, sc = e.resolve(collection, renderer['m_Sprite'])
    _, tk, tc = e.resolve(sc, sprite['m_RD']['m_Texture'])
    png = tk.removesuffix('.json') + '.png'
    raw = client.request('/Assets/Image', Path=json.dumps({'C': tc, 'D': sprite['m_RD']['m_Texture']['m_PathID']}), Extension='png')
    (output / png).write_bytes(raw)
    e.hashes[png] = hashlib.sha256(raw).hexdigest()
    fsm = decode_fsm(e.cache[root['PlayMakerFSM'][0]]['m_Structure']['fsm'])
    (output / 'dispatch.json').write_text(json.dumps(fsm, indent=2)+'\n')
    mapping = dict(build=22529139, owner=8400, root=root, components=parts,
                   sprite=sk, image=png, source_sha256=e.hashes)
    (output / 'damage-flash.json').write_text(json.dumps(mapping, indent=2)+'\n')
    extract_flash_animation(client, collection, output)
    print(json.dumps(dict(owner=8400, files=len(e.hashes))))
    if include_flings:
        extract_flings(client, collection, fsm)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--flings-only', action='store_true')
    parser.add_argument('--flash-animation-only', action='store_true')
    args = parser.parse_args()
    extract(args.flings_only, args.flash_animation_only)
