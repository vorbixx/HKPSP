"""Original Archives multizap silhouettes, repacked without pixel/geometry changes."""
import argparse
import importlib
import json
from pathlib import Path
import sys
import struct
import converted_files
from assetripper_client import ROOT
from watcher_atlas import repack
from uumuu_blood_art import append_blood
from tool_paths import TEX3DS as TEX3DS_EXE
STEM='uumuu-effects'
ALIASES={'Lightning':('sharedassets143.assets',14,{n:n for n in ('Zap Antic','Zap','Zap End')})}
def validate_snapshot(snapshot):
 data=(snapshot/f'{STEM}-art.bin').read_bytes()
 magic,pages,sprites,clips,frames=struct.unpack_from('<4s4I',data)
 if (magic,pages,sprites,clips,frames)!=(b'HKA1',1,19,3,19):
  raise ValueError('Original lightning bank structure changed')
 expected=(('Zap Antic',12.,1,11),('Zap',12.,1,5),('Zap End',12.,2,3))
 offset=20+sprites*36
 for index,(name,fps,wrap,count) in enumerate(expected):
  record=struct.unpack_from('<48sf4I',data,offset+index*68)
  if (record[0].rstrip(b'\0').decode(),record[1],record[2],record[5])!=(f'Lightning/{name}',fps,wrap,count):
   raise ValueError('Original lightning animation changed: '+name)
 return True
def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--extract',action='store_true')
 p.add_argument('--source',type=Path,default=ROOT/'_private/original/Hollow Knight/hollow_knight_Data')
 p.add_argument('--snapshot',dest='snapshot_dir',type=Path,default=ROOT/'_private/minimum-bosses/uumuu-effects-art')
 p.add_argument('--output',type=Path,default=ROOT/'_private/converted/uumuu-effects')
 p.add_argument('--packages',type=Path,default=ROOT/'_private/tools/boss-python')
 p.add_argument('--tex3ds',type=Path,default=Path(TEX3DS_EXE))
 a=p.parse_args()
 if a.extract:
  sys.path.insert(0,str(a.packages))
  from watcher_unity import AlignedGenerator
  importlib.import_module('UnityPy.helpers.TypeTreeGenerator').TypeTreeGenerator=AlignedGenerator
  from convert_crossroads_bosses import convert
  a.scale,a.snapshot,a.save_snapshot=1,None,a.snapshot_dir
  a.packages=str(a.packages)
  convert(a,aliases=ALIASES,stem=STEM,component_scenes=[],split_oversized=False,complete_library=False)
 else:
  validate_snapshot(a.snapshot_dir)
  repack(a.snapshot_dir,a.output,a.tex3ds,stem=STEM)
  m=json.loads((a.output/'manifest.json').read_text())
  # Fail before runtime loading if source art exceeds the scene-local allocation.
  if m['page_dimensions']!=[[1024,1024]]:raise ValueError('Uumuu lightning allocation changed')
  blood_bytes=append_blood(a.output/'romfs',ROOT/'_private/research/false-knight-effects',a.tex3ds,
                           ROOT/'include/uumuu_blood_data.hpp',ROOT/'_private/minimum-bosses/uumuu/corpse-source.json')
  m['page_dimensions'].append([128,128]);m['pages']+=1;m['texture_bytes']+=blood_bytes
  m['limitations']=['Pooled chase-zap particles are separate from these arena animations.']
  (a.output/'manifest.json').write_text(json.dumps(m,indent=2)+'\n')
if __name__=='__main__':main()
