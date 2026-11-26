"""Source-bound Greenpath bench tolls; either machine broadcasts to both gates."""
import json
import sys
from pathlib import Path
import struct
import xml.etree.ElementTree as ET
from convert_route_tolls import (ROOT, DATA, OUT, Asset, owner, region,
                                  compile_actor, language_asset, decrypt_language)
from inspect_fsm import decode_fsm

def main():
    scene='Fungus1_31';level=158
    source=ROOT/f'_private/research/crossroads-scenes/{scene}/terrain'
    read=lambda i:json.loads((source/f'level158-{i}.json').read_text())
    a=Asset(DATA/'level158')
    resources=Asset(DATA/'resources.assets');_,encoded=language_asset(resources.objects,'EN_Prices')
    prices={e.attrib['name']:e.text for e in ET.fromstring(decrypt_language(encoded))}
    price=int(prices['STAGWAY_CROSSROADS']);assert price==50
    machines=(1811,686);triggers=(6345,6286);fsms=(9261,9213)
    for identity,fsm_id in zip(machines,fsms):
        fsm=decode_fsm(read(fsm_id)['m_Structure']['fsm'])
        assert fsm['name']=='Toll Machine' and fsm['start']=='Pause'
        variables={v['name']:v['value'] for key in ('boolVariables','stringVariables') for v in fsm['variables'][key]}
        assert not variables['Activated'] and variables['Price String Name']=='STAGWAY_CROSSROADS'
        opening=next(s for s in fsm['states'] if s['name']=='Open Gates')
        assert any(v.get('sendEvent',{}).get('value')=='TOLL GATE OPEN' and v['eventTarget']['target']==4
                   for action in opening['actions'] if action['enabled'] for v in [action['parameters']])
    gates=(6365,6388);gate_owners=[owner(a.objects[i][1]) for i in gates]
    regions=[region(a,i) for i in triggers]
    # Both source gates remove themselves at the end of Gate Open.
    # Gate duration is supplied by the original clip, not an invented collision timer.
    sys.path.append(str(ROOT/'_private/tools/boss-python'))
    import UnityPy
    from watcher_unity import AlignedGenerator
    env=UnityPy.Environment(path=str(DATA))
    generator=AlignedGenerator('6000.0.61f1');generator.load_local_dll_folder(str(DATA/'Managed'))
    env.typetree_generator=generator
    env.load_file(str(DATA/'level158'))
    scene_asset=next(f for f in env.files.values() if getattr(f,'name',None)=='level158')
    animator=scene_asset.objects[9163].read_typetree();ref=animator['library']
    library_name=Path(scene_asset.externals[ref['m_FileID']-1].path).name
    env.load_file(str(DATA/library_name))
    library_asset=next(f for f in env.files.values() if getattr(f,'name',None)==library_name)
    library=library_asset.objects[ref['m_PathID']].read_typetree()
    clip=next(c for c in library['clips'] if c['name']=='Gate Open')
    seconds=len(clip['frames'])/clip['fps']
    data=struct.pack('<4s4I2f12f',b'RTS4',3,price,158001811,2,0,seconds,*([0]*12))
    for r in regions:data+=struct.pack('<4f',*r)
    for ids in (gate_owners,[],list(gates),[],[],[]):
        data+=struct.pack('<I',len(ids))+struct.pack('<'+'I'*len(ids),*ids)
    data+=struct.pack('<I',0) # Geometry is already admitted by the scene terrain converter.
    poses=[]
    for identity in machines:
        t=a.transform(identity);p=a.point(t,(0,0));x=a.point(t,(1,0));y=a.point(t,(0,1))
        assert abs(x[1]-p[1])+abs(y[0]-p[0])<1e-5
        poses.append((*p,x[0]-p[0],y[1]-p[1]))
    data+=struct.pack('<I',2)+b''.join(struct.pack('<4f',*p) for p in poses)
    data+=struct.pack('<2I',*(158000000+i for i in machines))
    OUT.mkdir(parents=True,exist_ok=True);(OUT/(scene+'.bin')).write_bytes(data)
    compile_actor(ROOT/'_private/converted/greenpath-tolls',level=level,actor_id=1811,
                  animator_id=9155,actor_name=a.gos[1811]['name'],stem='greenpath-toll-actor',
                  clip_names=('Box_idle',),atlas_size=1024,fit_atlas=True)
    (OUT/(scene+'-source.json')).write_text(json.dumps(dict(scene=scene,sha256=a.sha256,
        price=price,machines=machines,regions=regions,gates=gates,opening_seconds=seconds),indent=2)+'\n')
    print('Greenpath tolls:50 Geo, two independent payments, broadcast opens both gates')
if __name__=='__main__':main()
