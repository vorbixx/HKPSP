"""Export only the original Lantern prefab/clip/sprites; run under validation-lock -Exclusive."""
import sys,json,hashlib,os
from pathlib import Path
root=Path(__file__).resolve().parents[1]
sys.path.append(str(root/'_private/tools/boss-python'))
if os.environ.get('HK3DS_UNITYPY_PATH'):
 sys.path.insert(0,os.environ['HK3DS_UNITYPY_PATH'])
import UnityPy
source=root/'_private/original/Hollow Knight/hollow_knight_Data/resources.assets'
env=UnityPy.load(str(source)); f=next(f for f in env.files.values() if getattr(f,'name',None)=='resources.assets')
out=root/'_private/research/lantern-glow';out.mkdir(parents=True,exist_ok=True)
seen=set()
def export(i,recurse=False):
 if i in seen:return
 seen.add(i);o=f.objects[i];
 if o.type.name=='MonoBehaviour':return
 d=o.read_typetree();(out/f'resources.assets-{i}.json').write_text(json.dumps(d,indent=2,default=list)+'\n')
 print(i,o.type.name,d.get('m_Name',''))
 if o.type.name=='Sprite':o.read().image.save(out/f'sprite-{i}.png')
 if recurse:
  if o.type.name=='GameObject':
   for c in d['m_Component']:export(c['component']['m_PathID'],True)
  if o.type.name=='Transform':
   for c in d['m_Children']:export(c['m_PathID'],True)
   export(d['m_GameObject']['m_PathID'],True)
assert f.objects[7964].read_typetree()['m_Name']=='Lantern'
export(7964,True)
for i in [1132,1959,2602]:export(i)
material=f.objects[16418].read().m_Materials[0].deref()
assert material.read().m_Name=='Sprites-Default'
(out/'material.json').write_text(json.dumps(material.read_typetree(),indent=2,default=list)+'\n')
(out/'source.json').write_text(json.dumps({'source':str(source),'sha256':hashlib.sha256(source.read_bytes()).hexdigest()},indent=2)+'\n')
