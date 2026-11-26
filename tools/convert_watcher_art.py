"""Pack the original Watcher clips at source scale 1; never invent frames."""
import converted_files  # Install copy-on-write protection before output writes.
import argparse
import importlib
import json
from pathlib import Path
import sys
from assetripper_client import ROOT
from tool_paths import TEX3DS as TEX3DS_EXE


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--extract',action='store_true')
    parser.add_argument('--source',type=Path,default=ROOT/'_private/original/Hollow Knight/hollow_knight_Data')
    parser.add_argument('--snapshot',type=Path,default=ROOT/'_private/minimum-bosses/art')
    parser.add_argument('--output',type=Path,default=ROOT/'_private/converted/watcher-knights')
    parser.add_argument('--packages',type=Path,default=ROOT/'_private/minimum-bosses/packages')
    parser.add_argument('--tex3ds',type=Path,default=Path(TEX3DS_EXE))
    args=parser.parse_args()
    snapshot=args.snapshot
    args.scale=1
    args.save_snapshot=None
    aliases={}
    if args.extract:
        sys.path.insert(0,str(args.packages))
        from watcher_unity import AlignedGenerator
        importlib.import_module('UnityPy.helpers.TypeTreeGenerator').TypeTreeGenerator=AlignedGenerator
        source=json.loads((ROOT/'_private/minimum-bosses/watcher-boss-source.json').read_text())
        knight=next(a for a in source['actors'] if a['name']=='Black Knight 1')
        animator=next(c for c in knight['components'] if c['type']=='tk2dSpriteAnimator')
        clips=animator['animation_library']['clips']
        aliases={'Watcher':('sharedassets115.assets',174,{c['name']:c['name'] for c in clips if c['name'] and c['frames']})}
        args.save_snapshot=snapshot
        args.snapshot=None
    if args.extract:
        from convert_crossroads_bosses import convert
        convert(args,aliases=aliases,stem='watcher',component_scenes=[])
        manifest_path=args.output/'manifest.json'
        manifest=json.loads(manifest_path.read_text())
        manifest['limitations']=['Original clips only; particles, audio, title and corpse presentation remain separate.',
                                 'Source scale 1; no replacement sprites or downsampling.']
        text=json.dumps(manifest,indent=2)
        manifest_path.write_text(text)
        (snapshot/'manifest.json').write_text(text)
    from watcher_atlas import repack
    repack(snapshot,args.output,args.tex3ds)


if __name__=='__main__':main()
