"""Export only the normal hero death prefab and its presentation dependencies."""
import hashlib
import json
from assetripper_client import Client, ROOT
from extract_crawler import Export
from inspect_fsm import decode_fsm


def extract(client=None, output=None):
    client = client or Client()
    output = output or ROOT / '_private/research/hero-death'
    output.mkdir(parents=True, exist_ok=True)
    e = Export(client, output)
    row = next(r for r in client.search('Knight') if r['type'] == 'GameObject'
               and r['name'] == 'Knight' and r['collection'] == 'resources.assets')
    collection = row['path']['C']
    nodes, textures = [], {}

    def image(c, ref):
        data, key, tc = e.resolve(c, ref)
        target = key.removesuffix('.json') + '.png'
        if target not in textures:
            raw = client.request('/Assets/Image', Path=json.dumps({'C': tc, 'D': ref['m_PathID']}), Extension='png')
            (output / target).write_bytes(raw)
            e.hashes[target] = hashlib.sha256(raw).hexdigest()
            textures[target] = True
        return key, target

    def walk(asset, parent=None):
        parts = e.components(collection, asset)
        go = e.cache[parts['GameObject'][0]]
        tr = e.cache[parts['Transform'][0]]
        node = dict(name=go['m_Name'], id=asset, parent=parent, components=parts)
        nodes.append(node)
        for key in parts.get('PlayMakerFSM', []):
            (output / (key.removesuffix('.json') + '-fsm.json')).write_text(
                json.dumps(decode_fsm(e.cache[key]['m_Structure']['fsm']), indent=2))
        if go['m_Name'] in ('Particle Wave', 'Shade Particles', 'Ash L', 'Ash R', 'Corpse Nail Hero',
                            'Death Crack', 'Hit Crack'):
            for key in parts.get('SpriteRenderer', []):
                sprite, sk, sc = e.resolve(collection, e.cache[key]['m_Sprite'])
                node['sprite'] = sk
                node['texture'], node['image'] = image(sc, sprite['m_RD']['m_Texture'])
            for key in parts.get('ParticleSystemRenderer', []):
                material, mk, mc = e.resolve(collection, e.cache[key]['m_Materials'][0])
                node['material'] = mk
                envs = material['m_SavedProperties']['m_TexEnvs']
                texture = envs['_MainTex']['m_Texture']
                node['texture'], node['image'] = image(mc, texture)
        for child in tr['m_Children']:
            t, _, _ = e.resolve(collection, child)
            walk(t['m_GameObject']['m_PathID'], asset)

    for asset in (7512, 6648, 6436):
        walk(asset)
    # CameraFade's enabled SetFsmFloat actions set HUD Blanker to .75 / .5 s.
    template, _ = e.fetch(collection, 20521)
    (output / 'camera-fade-template.json').write_text(json.dumps(
        decode_fsm(template['m_Structure']['fsm']), indent=2))
    flashes = []
    for asset, name in ((1133, 'Death Flash'), (1131, 'Death Flash Rays')):
        clip, key = e.fetch(collection, asset)
        frames = []
        for frame in clip['m_PPtrCurves'][0]['m_Curve']:
            sprite, sk, sc = e.resolve(collection, frame['m_Value'])
            texture, png = image(sc, sprite['m_RD']['m_Texture'])
            frames.append(dict(time=frame['m_Time'], sprite=sk, texture=texture, image=png))
        flashes.append(dict(name=name, animation=key, frames=frames))
    audio = {}
    for asset in (1293, 1267, 1280):
        clip, key = e.fetch(collection, asset)
        name = clip['m_Name']
        raw = client.request('/Assets/Audio', Path=json.dumps({'C': collection, 'D': asset}))
        target = name + '.ogg'
        (output / target).write_bytes(raw)
        e.hashes[target] = hashlib.sha256(raw).hexdigest()
        audio[name] = target
    (output / 'death-map.json').write_text(json.dumps(
        dict(build=22529139, nodes=nodes, flashes=flashes, audio=audio, source_sha256=e.hashes), indent=2))
    print(json.dumps(dict(nodes=len(nodes), images=len(textures), audio=list(audio))))


if __name__ == '__main__':
    extract()
