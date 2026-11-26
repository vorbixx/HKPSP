"""Export the six ordinary-route hero emitters from the copied original data."""
import hashlib
import json
from assetripper_client import Client, ROOT
from extract_crawler import Export

OWNERS = (4217, 6465, 7661, 8103, 8243, 3992)


def extract(client=None, output=None):
    client = client or Client()
    output = output or ROOT / '_private/research/hero-effects'
    output.mkdir(parents=True, exist_ok=True)
    row = next(r for r in client.search('Knight') if r['type'] == 'GameObject'
               and r['name'] == 'Knight' and r['collection'] == 'resources.assets')
    collection = row['path']['C']
    e = Export(client, output)
    nodes = []
    for owner in OWNERS:
        parts = e.components(collection, owner)
        transforms = []
        transform = parts['Transform'][0]
        while True:
            transforms.append(transform)
            tr = e.cache[transform]
            go, _, _ = e.resolve(collection, tr['m_GameObject'])
            if go['m_Name'] in ('Jump Effects', 'Damage Effect', 'BackdashTriple Effect'):
                break
            _, transform, _ = e.resolve(collection, tr['m_Father'])
        renderer = e.cache[parts['ParticleSystemRenderer'][0]]
        material, mk, mc = e.resolve(collection, renderer['m_Materials'][0])
        _, shader, _ = e.resolve(mc, material['m_Shader'])
        tex = material['m_SavedProperties']['m_TexEnvs']['_MainTex']['m_Texture']
        _, tk, tc = e.resolve(mc, tex)
        png = tk.removesuffix('.json') + '.png'
        raw = client.request('/Assets/Image', Path=json.dumps({'C': tc, 'D': tex['m_PathID']}), Extension='png')
        (output / png).write_bytes(raw)
        e.hashes[png] = hashlib.sha256(raw).hexdigest()
        nodes.append(dict(owner=owner, components=parts, transforms=transforms,
                          material=mk, shader=shader, image=png))
    (output / 'hero-effects.json').write_text(json.dumps(dict(build=22529139, nodes=nodes,
        source_sha256=e.hashes), indent=2) + '\n')
    print(json.dumps(dict(owners=list(OWNERS), files=len(e.hashes))))


if __name__ == '__main__':
    extract()
