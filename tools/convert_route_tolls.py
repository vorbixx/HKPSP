"""Compile the City lift and Mines toll regions, branches and geometry from original exports."""
import converted_files
import json, struct
from pathlib import Path
from pickup_assets import Asset
from convert_sly_actor import compile_actor
from world_quest_assets import language_asset
from extract_tutorial_prompts import decrypt_language
import xml.etree.ElementTree as ET
ROOT=Path(__file__).resolve().parents[1]
DATA=ROOT/'_private/original/Hollow Knight/hollow_knight_Data'
OUT=ROOT/'_private/converted/hero/romfs/route-tolls'
def descendants(a, owner):
    return sorted(set([owner]+list(a.descendants(a.transform(owner)))))
def owner(raw):return struct.unpack_from('<q',raw,4)[0]
def points(a, identity, scene, level):
    kind, raw=a.objects[identity];o=owner(raw);t=a.transform(o)
    d=json.loads((ROOT/f'_private/research/crossroads-scenes/{scene}/terrain/level{level}-{identity}.json').read_text())
    d=d.get('m_Structure',d)
    if d['m_IsTrigger']:return []
    if kind==61:
        x,y=struct.unpack_from('<2f',raw,72);w,h=struct.unpack_from('<2f',raw,136)
        local=[(x-w/2,y-h/2),(x+w/2,y-h/2),(x+w/2,y+h/2),(x-w/2,y+h/2)]
    elif kind==60:
        paths=d['m_Points']['m_Paths'];assert len(paths)==1
        path=paths[0]
        if isinstance(path,dict):path=path.get('Array',path.get('m_Points',path.get('points')))
        local=[(p.get('m_X',p.get('x')),p.get('m_Y',p.get('y'))) for p in path]
        offset=d['m_Offset'];local=[(x+offset['m_X'],y+offset['m_Y']) for x,y in local]
    else:raise ValueError('Unsupported toll shape')
    world=[a.point(t,p)[:2] for p in local]
    area=sum(world[i][0]*world[(i+1)%len(world)][1]-world[(i+1)%len(world)][0]*world[i][1] for i in range(len(world)))
    if area<0:world.reverse()
    return world
def region(a, identity):
    raw=a.objects[identity][1];x,y=struct.unpack_from('<2f',raw,72);w,h=struct.unpack_from('<2f',raw,136);t=a.transform(owner(raw))
    p=[a.point(t,(x+sx*w/2,y+sy*h/2)) for sx in (-1,1) for sy in (-1,1)]
    return [min(v[0] for v in p),min(v[1] for v in p),max(v[0] for v in p),max(v[1] for v in p)]
def compile_scene(scene,level,kind,price):
    a=Asset(DATA/f'level{level}')
    expected={80:'c25b18e092033c8c8a9b8594a66fe8b42a52b75eca787d340712612fdd376983',272:'c3f98c47355adfb0ce3a9800ba2aaaf03609ef316ad87e1e959b0894b6bf336d'}
    assert a.sha256==expected[level],'Unreviewed source build'
    def shapes(owners):return sorted(i for i,(k,r) in a.objects.items() if k in (60,61) and owner(r) in owners)
    if kind==1:
        hide_paid=sorted(set(descendants(a,219)+descendants(a,258)));hide_unpaid=descendants(a,213)
        moving=descendants(a,215);moving_shapes=shapes(moving)
        upper=a.point(a.transform(215),(0,0))[1];lower=9.02 # Lift lower FSM845 Bottom Y.
        regions=[region(a,559)];source=80000029
        entrance=region(a,569);lever=region(a,580);hero_check=region(a,573)
        for bounds in (entrance,lever,hero_check):
            bounds[1]+=lower-upper;bounds[3]+=lower-upper
        geometry=[(i,owner(a.objects[i][1]),points(a,i,scene,level)) for i in moving_shapes]
        geometry=[row for row in geometry if row[2]]
    else:
        hide_paid=sorted(set(descendants(a,398)+descendants(a,548)+[380,558]));hide_unpaid=[]
        moving=[];moving_shapes=[];upper=lower=0;regions=[region(a,2029),region(a,2045)];source=272000380;geometry=[];entrance=lever=hero_check=[0,0,0,0]
    lists=[hide_paid,hide_unpaid,shapes(hide_paid),shapes(hide_unpaid),moving,moving_shapes]
    data=struct.pack('<4s4I2f12f',b'RTS4',kind,price,source,len(regions),lower,upper,*entrance,*lever,*hero_check)
    for r in regions:data+=struct.pack('<4f',*r)
    for ids in lists:data+=struct.pack('<I',len(ids))+struct.pack('<'+'I'*len(ids),*ids)
    data+=struct.pack('<I',len(geometry))
    for identity,o,p in geometry:
        data+=struct.pack('<3I',identity,o,len(p))+b''.join(struct.pack('<2f',*xy) for xy in p)
    # The enabled toll renderers are tk2d meshes, omitted by SpriteRenderer extraction.
    actors = (29,) if kind == 1 else (380,558)
    poses=[]
    for actor in actors:
        transform=a.transform(actor)
        p=a.point(transform,(0,0));x=a.point(transform,(1,0));y=a.point(transform,(0,1))
        if abs(x[1]-p[1])+abs(y[0]-p[0])>1e-5:
            raise ValueError('Unsupported rotated toll pedestal')
        poses.append((*p,x[0]-p[0],y[1]-p[1]))
    data+=struct.pack('<I',len(poses))+b''.join(struct.pack('<4f',*pose) for pose in poses)
    compile_actor(ROOT/'_private/converted/route-tolls',level=level,actor_id=actors[0],
                  animator_id=810 if kind==1 else 2978,actor_name=a.gos[actors[0]]['name'],
                  stem='city-toll-actor' if kind==1 else 'mines-toll-actor',
                  clip_names=('Box_idle','Box_disappear') if kind==1 else ('Box_idle',),atlas_size=1024,fit_atlas=True)
    OUT.mkdir(parents=True,exist_ok=True);(OUT/(scene+'.bin')).write_bytes(data)
    return dict(scene=scene,level=level,sha256=a.sha256,price=price,regions=regions,lists=lists,geometry_shapes=[r[0] for r in geometry])
def main():
    resources=Asset(DATA/'resources.assets');_,encoded=language_asset(resources.objects,'EN_Prices')
    prices={e.attrib['name']:e.text for e in ET.fromstring(decrypt_language(encoded))}
    city=int(prices['CITY_LIFT_1']);mines=int(prices['TOLL_GATE_MINES']);assert (city,mines)==(150,50)
    rows=[compile_scene('Crossroads_49b',80,1,city),compile_scene('Mines_33',272,2,mines)]
    (OUT/'source.json').write_text(json.dumps(rows,indent=2));print('Compiled City lift 150 Geo; paired Mines toll 50 Geo with Lantern condition')
if __name__=='__main__':main()
