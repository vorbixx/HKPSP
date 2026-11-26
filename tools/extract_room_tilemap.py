"""Export only the serialized Scenemap TileMap meshes and their dependencies."""
import argparse
import hashlib
import json
from pathlib import Path
from assetripper_client import Client, ROOT
from extract_crawler import Export, write_asset


def extract_tilemap(e, collection='level6'):
    result = []
    for row in e.client.search('MeshRenderer'):
        if row['collection'] != collection: continue
        renderer, rk = e.fetch(row['path']['C'], row['path']['D'])
        go, gk, gc = e.resolve(row['path']['C'], renderer['m_GameObject'])
        if not go['m_Name'].startswith('Chunk '): continue
        parts = [e.resolve(gc, c['m_Component']) for c in go['m_Components']]
        transform, tk, tc = next(p for p in parts if 'm_LocalPosition' in p[0])
        chain, owners, names = [], [], []
        while True:
            chain.append(tk)
            owner, ok, _ = e.resolve(tc, transform['m_GameObject'])
            owners.append(ok); names.append(owner['m_Name'])
            if not transform['m_Father']['m_PathID']: break
            transform, tk, tc = e.resolve(tc, transform['m_Father'])
        if collection=='level6' and names[1:] != ['Scenemap', 'TileMap Render Data']: continue
        mesh_filter, _, fc = next(p for p in parts if 'm_Mesh' in p[0])
        _, mesh, _ = e.resolve(fc, mesh_filter['m_Mesh'])
        if len(renderer['m_Materials']) != 1: raise ValueError('Tilemap material count')
        material, mk, mc = e.resolve(gc, renderer['m_Materials'][0])
        _, shader, _ = e.resolve(mc, material['m_Shader'])
        env = material['m_SavedProperties']['m_TexEnvs']['_MainTex']
        _, texture, texc = e.resolve(mc, env['m_Texture'])
        png = texture.removesuffix('.json') + '.png'
        raw = e.client.request('/Assets/Image', Path=json.dumps({'C':texc,'D':env['m_Texture']['m_PathID']}), Extension='png')
        write_asset(e.output/png, raw); e.hashes[png] = hashlib.sha256(raw).hexdigest()
        result.append(dict(renderer=rk, go=gk, chain=chain, mesh=mesh, material=mk,
                           shader=shader, texture=texture, image=png,
                           active=bool(renderer['m_Enabled']) and all(e.cache[k]['m_IsActive'] for k in owners)))
    if not result and collection=='level6': raise ValueError('No Scenemap TileMap meshes found')
    return result


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port', type=int, default=58291)
    p.add_argument('--output', type=Path, default=ROOT/'_private/research/room-visuals')
    a = p.parse_args()
    if not a.output.resolve().is_relative_to(ROOT/'_private'): raise ValueError('Export must remain private')
    path = a.output/'room-visuals-map.json'; mapping = json.loads(path.read_text())
    e = Export(Client(a.port), a.output)
    mapping['tilemap'] = extract_tilemap(e); mapping['source_sha256'].update(e.hashes)
    path.write_text(json.dumps(mapping, indent=2), encoding='utf-8')
    print(json.dumps(dict(tilemap_meshes=len(mapping['tilemap']), files=len(e.hashes))))
