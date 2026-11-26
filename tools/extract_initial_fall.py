"""Export initial impact dust and flock only, from the copied Tutorial_01 assets."""
import json
from assetripper_client import Client, ROOT
from extract_decor_effects import Store
from extract_crawler import native_component_kind


def extract():
    output = ROOT / '_private/research/initial-fall'
    output.mkdir(parents=True, exist_ok=True)
    store = Store(output, Client())
    objects, groups = {}, {}

    def visit(key, group):
        if key in objects:
            return
        go = store.get(key)
        parts = {}
        objects[key] = dict(name=go['m_Name'], components=parts)
        for ref in go['m_Components']:
            ck, component = store.resolve(key, ref['m_Component'])
            kind = native_component_kind(component)
            if 'm_Script' in component:
                _, script = store.resolve(ck, component['m_Script'])
                kind = script['m_ClassName']
            parts.setdefault(kind or 'Other', []).append(ck)
            if 'm_LocalPosition' in component:
                ancestor = component
                ak = ck
                while ancestor['m_Father']['m_PathID']:
                    ak, ancestor = store.resolve(ak, ancestor['m_Father'])
                for child in component['m_Children']:
                    tk, tr = store.resolve(ck, child)
                    owner, _ = store.resolve(tk, tr['m_GameObject'])
                    visit(owner, group)
            if kind == 'ParticleSystemRenderer':
                for material in component['m_Materials']:
                    if not material['m_PathID']:
                        continue
                    mk, data = store.resolve(ck, material)
                    store.resolve(mk, data['m_Shader'])
                    tex = data['m_SavedProperties']['m_TexEnvs']['_MainTex']['m_Texture']
                    texture, _ = store.resolve(mk, tex)
                    store.binary(texture, 'texture')
            if kind == 'tk2dSpriteAnimator':
                lk, library = store.resolve(ck, component['m_Structure']['library'])
                clip = library['m_Structure']['clips'][component['m_Structure']['defaultClipId']]
                for frame in clip['frames']:
                    sk, sprites = store.resolve(lk, frame['spriteCollection'])
                    sprite = sprites['m_Structure']['spriteDefinitions'][frame['spriteId']]
                    texture, _ = store.resolve(sk, sprites['m_Structure']['textures'][sprite['materialId']])
                    store.binary(texture, 'texture')
        if 'ParticleSystem' in parts:
            groups.setdefault(group, []).append(key)

    # Export the default dust branch used by this presentation. The ground's
    # exact environment selection still needs an original-scene comparison.
    for root in (4220,4329,4973):
        visit(f'resources.assets-{root}.json', 'land')
    visit('level6-3218.json', 'flock')
    for key in ('resources.assets-4666.json','resources.assets-26788.json','level6-12276.json'):
        store.get(key)
    hero = store.get('resources.assets-20600.json')
    if json.loads((ROOT / '_private/research/assets/c1-20600.json').read_text()) != hero:
        raise ValueError('Source build differs from 22529139')
    mapping = dict(build=22529139, objects=objects, groups=groups,
                   dependency_tables=store.tables, media=store.media, source_sha256=store.hashes)
    (output / 'initial-fall.json').write_text(json.dumps(mapping, indent=2), encoding='utf-8')
    print(json.dumps(dict(groups=groups, sources=len(store.hashes))))


if __name__ == '__main__':
    extract()
