"""Extract/repack the original Teacher's Archives battle gate sprites."""
import argparse
import importlib
import json
from pathlib import Path
import struct
import sys
import converted_files
from assetripper_client import ROOT
from watcher_atlas import repack
from tool_paths import TEX3DS as TEX3DS_EXE

STEM = 'uumuu-gate'
ALIASES = {'Gate': ('sharedassets229.assets', 80, {name: name for name in ('Closed', 'Open', 'Opened', 'Close')})}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--extract', action='store_true')
    parser.add_argument('--source', type=Path, default=ROOT/'_private/original/Hollow Knight/hollow_knight_Data')
    parser.add_argument('--snapshot', dest='snapshot_dir', type=Path, default=ROOT/'_private/minimum-bosses/uumuu-gate-art')
    parser.add_argument('--output', type=Path, default=ROOT/'_private/converted/uumuu-gate')
    parser.add_argument('--packages', type=Path, default=ROOT/'_private/tools/boss-python')
    parser.add_argument('--tex3ds', type=Path, default=Path(TEX3DS_EXE))
    args = parser.parse_args()
    if args.extract:
        from convert_uumuu_quirrel_art import _check_external_mapping
        _check_external_mapping(args.source, args.packages)
        sys.path.insert(0, str(args.packages))
        type_tree = importlib.import_module('UnityPy.helpers.TypeTreeGenerator')
        from watcher_unity import AlignedGenerator
        type_tree.TypeTreeGenerator = AlignedGenerator
        from convert_crossroads_bosses import convert
        args.scale, args.snapshot, args.save_snapshot = 1, None, args.snapshot_dir
        args.packages = str(args.packages)
        convert(args, aliases=ALIASES, stem=STEM, component_scenes=[], split_oversized=False)
        return
    data = (args.snapshot_dir/f'{STEM}-art.bin').read_bytes()
    if struct.unpack_from('<4s4I', data) != (b'HKA1', 1, 7, 4, 13):
        raise ValueError('Archive gate bank must retain seven source sprites and four clips')
    repack(args.snapshot_dir, args.output, args.tex3ds, stem=STEM)
    manifest = json.loads((args.output/'manifest.json').read_text())
    from uumuu_art_layout import validate_runtime_layout
    validate_runtime_layout(manifest, ((1024, 1024),))

if __name__ == '__main__':
    main()
