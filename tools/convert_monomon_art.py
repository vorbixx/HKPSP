"""Repack original Monomon/Archives actors as bounded scene-local sprite banks."""
import converted_files
import argparse
import importlib
import json
from pathlib import Path
import sys
from assetripper_client import ROOT
from watcher_atlas import repack
from tool_paths import TEX3DS as TEX3DS_EXE


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--extract', action='store_true')
    p.add_argument('--archive', action='store_true')
    p.add_argument('--source', type=Path, default=ROOT/'_private/original/Hollow Knight/hollow_knight_Data')
    p.add_argument('--packages', type=Path, default=ROOT/'_private/tools/boss-python')
    p.add_argument('--tex3ds', type=Path, default=Path(TEX3DS_EXE))
    a = p.parse_args()
    stem = 'monomon-archive' if a.archive else 'monomon-guardian'
    a.output = ROOT/'_private/converted'/stem
    snapshot = ROOT/'_private/minimum-bosses'/f'{stem}-art'
    if a.extract:
        sys.path.insert(0, str(a.packages))
        from watcher_unity import AlignedGenerator
        importlib.import_module('UnityPy.helpers.TypeTreeGenerator').TypeTreeGenerator = AlignedGenerator
        from convert_crossroads_bosses import convert
        # approximation: half-resolution pixels for the 400px screen; authored
        # world bounds and all clip frames/timings remain unchanged. Full-size
        # guardian pixels exceed the 6 MiB scene-local texture allocation.
        a.scale, a.snapshot, a.save_snapshot = .5, None, snapshot
        a.packages = str(a.packages)
        library = ('sharedassets178.assets', 21) if a.archive else ('sharedassets402.assets', 8)
        source_scene = 'level228' if a.archive else 'level402'
        actor_id = 1607 if a.archive else 84
        source = json.loads((ROOT/'_private/minimum-bosses/uumuu'/f'{source_scene}-source.json').read_text(encoding='utf-8'))
        actor = next(v for v in source['actors'] if v['path_id'] == actor_id)
        animator = next(c for c in actor['components'] if c['type'] == 'tk2dSpriteAnimator')
        clips = {c['name']: c['name'] for c in animator['animation_library']['clips']}
        aliases = {'Monomon': (*library, clips)}
        if not a.archive:
            # level402 actor86, library external16 = sharedassets46.assets.
            aliases['Glow'] = ('sharedassets46.assets', 60, {'Wounded': 'Wounded'})
        convert(a, aliases=aliases, stem=stem,
                component_scenes=[], split_oversized=False, complete_library=False)
    repack(snapshot, a.output, a.tex3ds, stem=stem)
    manifest = json.loads((a.output/'manifest.json').read_text(encoding='utf-8'))
    cost = sum(w*h*2 for w,h in manifest['page_dimensions'])
    if cost > 6*1024*1024:
        raise ValueError(f'{stem} exceeds its 6 MiB scene-local allocation: {cost}')
    print(f'{stem}: {cost} texture bytes; pages={manifest["page_dimensions"]}')


if __name__ == '__main__':
    main()
