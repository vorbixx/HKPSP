"""Export copied original Watcher arena/additive data; all output stays private."""
import argparse,sys,json,hashlib,re,struct
from pathlib import Path
root=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--scene',choices=('level113','level115'),default='level115')
parser.add_argument('--source',type=Path,default=root/'_private/original/Hollow Knight/hollow_knight_Data')
parser.add_argument('--output',type=Path,default=root/'_private/minimum-bosses')
parser.add_argument('--packages',type=Path,default=root/'_private/minimum-bosses/packages')
args=parser.parse_args()
sys.path.insert(0,str(args.packages))
import UnityPy
from watcher_unity import AlignedGenerator
from UnityPy.helpers import TypeTreeHelper
TypeTreeHelper.read_typetree_boost=None  # Boost caches stale alignment after flag correction.
from inspect_fsm import decode_fsm
scene_name=args.scene
stem='watcher-boss' if scene_name=='level115' else 'watcher'
source=args.source.resolve()
out=args.output.resolve()
if not out.is_relative_to((root/'_private').resolve()): parser.error('Output must remain private')
out.mkdir(parents=True,exist_ok=True)

env=UnityPy.Environment(path=str(source)); gen=AlignedGenerator('6000.0.61f1'); gen.load_local_dll_folder(str(source/'Managed')); env.typetree_generator=gen
files={}; hashes={str(p.relative_to(source)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted((source/'Managed').glob('*.dll'))}
def load(name):
    if name not in files:
        p=source/name; hashes[name]=hashlib.sha256(p.read_bytes()).hexdigest(); env.load_file(str(p)); files[name]=next(f for f in env.files.values() if getattr(f,'name',None)==name)
    return files[name]
def resolve(owner,ref):
    name=owner.name if ref['m_FileID']==0 else Path(owner.externals[ref['m_FileID']-1].path).name
    return load(name).objects[ref['m_PathID']]
scene=load(scene_name); entries=[]; names=[]
hierarchy={str(o.path_id):o.read_typetree() for o in scene.objects.values() if o.type.name=='Transform'}
selected=set()
def children(go,depth=0):
    if depth>20:raise ValueError('Deep hierarchy')
    for ent in go['m_Component']:
        c=resolve(scene,ent['component'])
        if c.type.name!='Transform':continue
        for ref in c.read_typetree()['m_Children']:
            child=resolve(c.assets_file,ref); child_go=resolve(child.assets_file,child.read_typetree()['m_GameObject'])
            selected.add(child_go.path_id); children(child_go.read_typetree(),depth+1)
for obj in list(scene.objects.values()):
    if obj.type.name=='GameObject':
        go=obj.read_typetree()
        if 'black knight' in go['m_Name'].lower():children(go)
for obj in list(scene.objects.values()):
    if obj.type.name!='GameObject':continue
    go=obj.read_typetree(); name=go['m_Name']; names.append(name)
    if obj.path_id not in selected and not any(x in name.lower() for x in ('black knight','battle','boss','door','chandelier','knight gate')):continue
    row=dict(name=name,path_id=obj.path_id,active=go['m_IsActive'],components=[])
    for entry in go['m_Component']:
        c=resolve(scene,entry['component']); typ=c.type.name
        try: data=c.read_typetree()
        except (ValueError,EOFError,IndexError,struct.error) as error:
            row['components'].append(dict(type=typ,path_id=c.path_id,unsupported=str(error)))
            print('Unsupported component',name,c.path_id,typ,str(error)); continue
        if 'm_Script' in data and data['m_Script']['m_PathID']:
            try: script=resolve(c.assets_file,data['m_Script']).read_typetree(); typ=script.get('m_ClassName',typ)
            except (IndexError,KeyError) as error:
                print('Invalid script reference',name,c.path_id,c.assets_file.name,data['m_Script'],'externals',len(c.assets_file.externals)); raise
        record=dict(type=typ,path_id=c.path_id,data=data)
        data=data.get('m_Structure',data)
        if 'fsm' in data:
            try: record['decoded']=decode_fsm(data['fsm'])
            except Exception as e: record['decode_error']=str(e)
        if typ=='tk2dSpriteAnimator':
            for key,value in data.items():
                if key.lower()=='library' and isinstance(value,dict) and value.get('m_PathID'):
                    record['animation_library']=resolve(c.assets_file,value).read_typetree()
        row['components'].append(record)
    entries.append(row)
physics=[]
for obj in load('globalgamemanagers').objects.values():
    if obj.type.name=='Physics2DSettings':physics.append(obj.read_typetree())
(out/'physics.json').write_text(json.dumps(physics,indent=2))
(out/f'{stem}-source.json').write_text(json.dumps(dict(build_id=int(re.search(r'\"buildid\"\s+\"(\d+)\"',(root/'_private/original/appmanifest_367520.acf').read_text()).group(1)),source_sha256=hashes,scene=scene_name,actors=entries),indent=2))
(out/f'{stem}-objects.json').write_text(json.dumps(names,indent=2))
(out/f'{stem}-transforms.json').write_text(json.dumps(hierarchy,indent=2))
print('Watcher actors:',len(entries),'source files:',len(hashes))
