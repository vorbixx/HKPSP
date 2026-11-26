"""Measure a Tutorial_01 shipping repack recipe for the current converted tables.

The shipped recipe (tools/psp/page-repack-tutorial.json) is bound byte for byte
to the tables converted from the prepared exports. A Steam export converts
King's Pass into a different tiling (865 instead of 940 sprites, 2026-11-25),
so this tool measures a recipe for that table the same way the earlier stages
did for one room:

1. replay the built-in Tutorial tape in a `host-build.ps1 -PageTrace` host
   (sprite, page and camera traces; tools/psp/page_set_report.py);
2. group every room sprite as used/unused by the trace and pack it against the
   recorded camera windows (tools/psp/mixed_page_report.py);
3. keep the shared-owner part of the base recipe, which must still match the
   current shared tables byte for byte.

    python -I tools/psp/build_tutorial_recipe.py --host build/psp/host-pagetrace/hkpsp-host.exe
        --output tools/psp/page-repack-tutorial-steam.json

The result is checked by `ship_page_repack.py --recipe <output> --output <dir>`
(every sprite byte-identical) before it is used.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import mixed_page_report
import ship_page_repack

BASE_RECIPE = ROOT / 'tools/psp/page-repack-tutorial.json'
TABLE = 'Tutorial_01/room-visual.psp.bin'
FRAMES = 3600  # the built-in Tutorial tape ends after 3,517 frames
LAYOUT_KEYS = ('after_pages', 'sprite_records', 'sprite_cluts', 'page_groups', 'index_sha256', 'clut_sha256')


def trace(host, assets, source, output, frames=FRAMES):
    subprocess.run([sys.executable, '-I', str(ROOT / 'tools/psp/page_set_report.py'), '--host', str(host),
                    '--assets', str(assets), '--source', str(source), '--trace', str(output),
                    '--frames', str(frames), '--output', str(output / 'report')], check=True)


def room_recipe(trace_dir, assets, margin):
    manifest = trace_dir / 'room-bank.tsv'
    manifest.write_text(f'Tutorial_01\t{(assets / TABLE).resolve()}\n', encoding='utf-8')
    report = mixed_page_report.analyze(trace_dir, manifest, 0, margin)
    bank = next(b for b in report['banks'] if b['room']) if 'banks' in report else None
    if bank is None:
        raise ValueError('Mixed layout report has no room bank')
    profile = dict(bank['profile'])
    room = dict(profile, layout=dict(profile=dict(profile), **{k: bank[k] for k in LAYOUT_KEYS}))
    return room, report


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--host', type=Path, help='host-build.ps1 -PageTrace executable (omit with --trace-dir)')
    parser.add_argument('--trace-dir', type=Path, default=ROOT / 'build/psp/tutorial-recipe-trace')
    parser.add_argument('--assets', type=Path, default=ROOT / '_private/converted-psp')
    parser.add_argument('--source', type=Path, default=ROOT)
    parser.add_argument('--base-recipe', type=Path, default=BASE_RECIPE)
    parser.add_argument('--margin-pixels', type=float, default=64)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    base = json.loads(args.base_recipe.read_text(encoding='utf-8'))
    shared_only = dict(base, room=dict(table_sha256='', **{k: v for k, v in base['room'].items() if k == 'groups'}))
    stale = [t for t in ship_page_repack.stale_tables_from(args.assets, shared_only) if t != TABLE]
    if stale:
        raise ValueError(f'Shared tables changed since the base recipe; rebase them first: {", ".join(stale)}')
    if args.host:
        trace(args.host, args.assets, args.source, args.trace_dir)
    room, report = room_recipe(args.trace_dir, args.assets, args.margin_pixels)
    recipe = dict(base, room=room)
    args.output.write_text(json.dumps(recipe, indent=1) + '\n', encoding='utf-8', newline='\n')
    print(f"Tutorial_01 recipe: {report['frames']} frames, before {report['before']}, after {report['after']}")


if __name__ == '__main__':
    main()
