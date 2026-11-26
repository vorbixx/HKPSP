"""Reproduce the private hero export through a running AssetRipper 2 server.

Load _private/analysis-input (including Managed) first. This tool resolves
session-specific collection IDs and writes canonical c1-<pathID>.json files;
c1 means resources.assets in our export, not a persistent AssetRipper index.
"""
import argparse
import json
from pathlib import Path
from assetripper_client import Client, ROOT


def extract(client, output):
    if not output.resolve().is_relative_to(ROOT / '_private'):
        raise ValueError('Original data must stay under _private')
    output.mkdir(parents=True,exist_ok=True)
    candidates = [r for r in client.search('Knight') if r['type']=='GameObject' and r['name']=='Knight' and r['collection']=='resources.assets']
    selected=[]
    for row in candidates:
        data=json.loads(client.asset(row['path']))
        if data['m_TagString']=='Player' and data['m_Layer']==9: selected.append((row,data))
    if len(selected)!=1: raise ValueError('Expected exactly one tagged Knight in resources.assets')
    row,hero=selected[0]
    collection=row['path']['C']
    seen={}
    def fetch(asset_id):
        if asset_id not in seen:
            raw=client.asset({'C':collection,'D':asset_id})
            seen[asset_id]=json.loads(raw)
            (output/f'c1-{asset_id}.json').write_bytes(raw)
        return seen[asset_id]
    fetch(row['path']['D'])
    for component in hero['m_Components']:
        ref=component['m_Component']
        if ref['m_FileID']!=0: raise ValueError('Unexpected external component')
        fetch(ref['m_PathID'])
    controller=next((v['m_Structure'] for v in seen.values() if 'RUN_SPEED' in v.get('m_Structure',{})),None)
    if not controller: raise ValueError('No decoded HeroController: load Managed assemblies with the input folder')
    animator=next(v['m_Structure'] for v in seen.values() if 'defaultClipId' in v.get('m_Structure',{}))
    library=fetch(animator['library']['m_PathID'])['m_Structure']
    for key in ['normalSlash','alternateSlash','upSlash','downSlash','wallSlash','normalSlashFsm','alternateSlashFsm','upSlashFsm','downSlashFsm','wallSlashFsm']:
        fetch(controller[key]['m_PathID'])
    combat = []
    for key in ['normalSlash', 'alternateSlash', 'upSlash', 'downSlash', 'wallSlash']:
        script_id = controller[key]['m_PathID']
        game_object_id = fetch(script_id)['m_GameObject']['m_PathID']
        components = [(r['m_Component']['m_PathID'], fetch(r['m_Component']['m_PathID']))
                      for r in fetch(game_object_id)['m_Components']]
        transform_id, transform = next((i,a) for i,a in components if 'm_LocalPosition' in a)
        collider_id, collider = next((i,a) for i,a in components if 'm_Points' in a)
        ancestors = []
        parent_id = transform['m_Father']['m_PathID']
        while parent_id:
            ancestors.append(parent_id)
            parent_id = fetch(parent_id)['m_Father']['m_PathID']
        # Keep the clash colliders and cancellation FSM available for the next
        # interaction systems, even though they are not damage hitboxes.
        for child in transform['m_Children']:
            child_transform = fetch(child['m_PathID'])
            child_object = fetch(child_transform['m_GameObject']['m_PathID'])
            for component in child_object['m_Components']:
                fetch(component['m_Component']['m_PathID'])
        combat.append(dict(kind=key, script=script_id, game_object=game_object_id,
                           transform=transform_id, collider=collider_id, ancestors=ancestors))
    (output/'combat-map.json').write_text(json.dumps(combat,indent=2))
    refs={(f['spriteCollection']['m_FileID'],f['spriteCollection']['m_PathID']) for clip in library['clips'] for f in clip['frames']}
    for file_id,asset_id in sorted(refs):
        if file_id!=0: raise ValueError('External sprite collection needs an explicit dependency map')
        sprites=fetch(asset_id)['m_Structure']
        for texture in sprites['textures']:
            if texture['m_FileID']!=0: raise ValueError('External texture needs an explicit dependency map')
            path={'C':collection,'D':texture['m_PathID']}
            (output/f"texture-{texture['m_PathID']}.png").write_bytes(client.request('/Assets/Image',Path=json.dumps(path),Extension='png'))
    for query in ['TimeManager','Physics2DSettings']:
        settings=[r for r in client.search(query) if r['type']==query and r['collection']=='globalgamemanagers']
        if len(settings)!=1: raise ValueError('Missing project settings: '+query)
        (output/f'{query}.json').write_bytes(client.asset(settings[0]['path']))
    (output/'export-map.json').write_text(json.dumps({'collection':'resources.assets','hero_path_id':row['path']['D'],
        'animation_library_path_id':animator['library']['m_PathID'],'asset_ids':sorted(seen),'assetripper_session_collection':collection},indent=2))
    print(f'Exported {len(seen)} assets and {len(refs)} sprite collections to {output}')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port',type=int,default=58291)
    parser.add_argument('--output',type=Path,default=ROOT/'_private/research/assets')
    args=parser.parse_args()
    extract(Client(args.port),args.output)
