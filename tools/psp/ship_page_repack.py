"""Assemble Tutorial_01 and the proved shared pool into a separate host asset tree.

No packing or quantization runs here. PPG1 follows SCL1 in pooled PSA1 tables:
four-byte magic, u32 ASCII byte count, then a safe logical page prefix.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import repack_pages as repack
import shared_page_repack as shared_repack

OWNERS = {
    'buzzer/buzzer-anim.psp.bin': 'buzzer/buzzer-anim.psp.bin',
    'crawler/crawler-anim.psp.bin': 'crawler/crawler-anim.psp.bin',
    'decor/decor-fx-visual.psp.bin': 'decor/decor-fx-visual.psp.bin',
    'fury/fury-visual.psp.bin': 'fury/fury-visual.psp.bin',
    'geo/geo-visual.psp.bin': 'geo/geo-visual.psp.bin',
    'great-door/great-door-visual.psp.bin': 'great-door/great-door-visual.psp.bin',
    'hud/hud-visual.psp.bin': 'hud/hud-visual.psp.bin',
    'motes/motes.psp.bin': 'motes/motes.psp.bin',
    'shade/shade-anim.psp.bin': 'shade/shade-anim.psp.bin',
    'route/hero.bin': 'hero/hero.psp.bin',
    'route/ability-panel.bin': 'ability/ability-panel.psp.bin',
}
UNCHANGED = ('fonts', 'hero-light', 'audio')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def pooled_table(raw, prefix='shared'):
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,64}', prefix):
        raise ValueError('Invalid shared page prefix')
    repack.read_bank(raw, with_cluts=True)
    if raw[:4] != b'PSA1' or b'SCL1' not in raw:
        raise ValueError('Pooled table requires PSA1 and per-sprite CLUTs')
    name = prefix.encode('ascii')
    return raw+struct.pack('<4sI', b'PPG1', len(name))+name


def verify_owner(baseline, packed):
    """Keep every baseline sprite ID, clip, frame and event when shipping."""
    _, before, before_end = repack.read_bank(baseline)
    _, after, after_end = repack.read_bank(packed, with_cluts=True)
    if baseline[:4] != b'PSA1' or packed[:4] != b'PSA1':
        raise ValueError('Shared owner requires animation tables')
    # Only atlas page count and sprite page/x/y may change. Comparing the
    # complete tail also protects unplayed clips and their frame/event IDs.
    if (baseline[:12] != packed[:12] or baseline[16:28] != packed[16:28] or
            [s[3:] for s in before] != [s[3:] for s in after]):
        raise ValueError('Shared owner baseline sprites or clip/frame counts changed')
    footer = len(packed)-8-4*len(after)
    if baseline[before_end:] != packed[after_end:footer]:
        raise ValueError('Shared owner baseline clips, frames or events changed')


def assemble(base, room, shared, output):
    base, room, shared, output = map(Path, (base, room, shared, output))
    if output.exists():
        raise ValueError('Shipping output must be a new directory')
    report = json.loads((shared/'repack.json').read_text(encoding='utf-8'))
    entries = report['banks']
    if len(entries) != len(OWNERS) or {b['bank'] for b in entries} != set(OWNERS):
        raise ValueError('Expected exactly the eleven Tutorial shared owners')
    room_raw = (room/'room-visual.psp.bin').read_bytes()
    # Tutorial is v1. Route-wide v2/tile adoption is explicitly deferred.
    if room_raw[:8] != struct.pack('<4sI', b'PSR1', 1):
        raise ValueError('Tutorial shipping requires the proved v1 room layout')
    repack.read_bank(room_raw, with_cluts=True)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='ship-repack-', dir=output.parent) as temp:
        staging = Path(temp)/'assets'
        staging.mkdir()
        for directory in sorted({Path(v).parts[0] for v in OWNERS.values()} | set(UNCHANGED)):
            shutil.copytree(base/directory, staging/directory)
        for table in sorted(base.glob('*/room-visual.psp.bin')):
            if table.parent.name != 'Tutorial_01':
                shutil.copytree(table.parent, staging/table.parent.name)
        shutil.copytree(room, staging/'Tutorial_01')
        (staging/'shared').mkdir()
        pages = sorted(shared.glob('page-*.hpt'))
        if not pages or {p.name for p in pages} != {f'page-{i}.hpt' for i in range(len(pages))}:
            raise ValueError('Shared page namespace is not contiguous')
        for source in [*pages, shared/'palettes.pcl']:
            shutil.copy2(source, staging/'shared'/source.name)
        for entry in entries:
            filename = entry['table']
            if Path(filename).name != filename:
                raise ValueError('Unsafe shared table filename')
            raw = (shared/filename).read_bytes()
            if struct.unpack_from('<I', raw, 12)[0] != len(pages):
                raise ValueError('Shared table/page count mismatch')
            _, records, _ = repack.read_bank(raw, with_cluts=True)
            if [list(s) for s in records] != entry['sprite_records']:
                raise ValueError('Shared records differ from proved export')
            target = staging/OWNERS[entry['bank']]
            verify_owner((base/OWNERS[entry['bank']]).read_bytes(), raw)
            target.write_bytes(pooled_table(raw))
            sidecar_root = shared/'sidecars'/Path(filename).stem
            for relative, expected in entry['sidecars_sha256'].items():
                source = shared/relative
                if not source.resolve().is_relative_to(sidecar_root.resolve()) or digest(source) != expected:
                    raise ValueError('Invalid shared sidecar')
                shutil.copy2(source, target.parent/source.name)
            # Owner pages are replaced by the single shared namespace.
            for old in target.parent.glob('page-*'):
                if old.suffix in ('.hpt', '.png'):
                    old.unlink()
        inventory = {p.relative_to(staging).as_posix(): digest(p)
                     for p in sorted(staging.rglob('*')) if p.is_file()}
        result = dict(scope='Tutorial_01 and shared owners only', shared_owners=len(entries),
                      shared_pages=len(pages), room_pages=struct.unpack_from('<I', room_raw, 12)[0],
                      files_sha256=inventory)
        (staging/'shipping-repack.json').write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
        staging.rename(output)
    return result


def generate(base, recipe, output):
    """Reproduce the measured Tutorial layout from hash-bound baseline assets."""
    base, output = Path(base).resolve(), Path(output).resolve()
    profile = json.loads(Path(recipe).read_text(encoding='utf-8'))
    shared_profile = profile['shared']
    for entry in shared_profile['banks']:
        relative = Path(entry['table'])
        if relative.is_absolute() or '..' in relative.parts:
            raise ValueError('Shared recipe requires a relative source table')
        entry['table'] = str(base/relative)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='repack-recipe-', dir=output.parent) as temp:
        temp = Path(temp)
        repack.convert_mixed(base/'Tutorial_01/room-visual.psp.bin',
                             profile['room'], temp/'room')
        shared_repack.export(shared_profile, temp/'shared')
        return assemble(base, temp/'room', temp/'shared', output)


def install(base, recipe):
    """Promote only Tutorial and shared owners, retaining all other room files."""
    base = Path(base).resolve()
    names = sorted({Path(v).parts[0] for v in OWNERS.values()} |
                   {'Tutorial_01', 'shared', 'shipping-repack.json'})
    with tempfile.TemporaryDirectory(prefix='install-repack-', dir=base.parent) as temp:
        temp = Path(temp)
        report = generate(base, recipe, temp/'assets')
        backup = temp/'backup'
        backup.mkdir()
        promoted = []
        try:
            for name in names:
                target = base/name
                if target.exists():
                    target.rename(backup/name)
                try:
                    (temp/'assets'/name).rename(target)
                except BaseException:
                    if (backup/name).exists():
                        (backup/name).rename(target)
                    raise
                promoted.append(name)
        except BaseException:
            for name in reversed(promoted):
                (base/name).rename(temp/'assets'/name)
                if (backup/name).exists():
                    (backup/name).rename(base/name)
            raise
        return report


def stale_tables_from(base, profile):
    """Owner tables whose bytes differ from the ones a measured recipe was made for."""
    base = Path(base).resolve()
    tables = [('Tutorial_01/room-visual.psp.bin', profile['room']['table_sha256'])]
    tables += [(entry['table'], entry['table_sha256']) for entry in profile['shared']['banks']]
    return [name for name, digest in tables
            if not (base/name).is_file() or hashlib.sha256((base/name).read_bytes()).hexdigest() != digest]


def stale_tables(base, recipe):
    return stale_tables_from(base, json.loads(Path(recipe).read_text(encoding='utf-8')))


def matching_recipe(base, recipes):
    """The first recipe measured for exactly these tables, or None with every mismatch."""
    mismatches = {}
    for recipe in recipes:
        stale = stale_tables(base, recipe)
        if not stale:
            return recipe, {}
        mismatches[recipe] = stale
    return None, mismatches


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', required=True, type=Path)
    for name in ('room', 'shared', 'output'):
        parser.add_argument('--'+name, type=Path)
    # Several recipes: the prepared and the Steam-converted tables each have one
    # (tools/psp/build_tutorial_recipe.py); the one measured for these tables is used.
    parser.add_argument('--recipe', type=Path, action='append', default=[])
    parser.add_argument('--install', action='store_true')
    # Tables converted from other game files than any measured recipe keep Tutorial_01
    # unpacked (correct art, more page loads on hardware) instead of failing.
    parser.add_argument('--if-matching', action='store_true',
                        help='skip with a warning when no recipe was measured for these tables')
    args = parser.parse_args()
    if args.recipe:
        if args.room or args.shared or (args.install == bool(args.output)):
            parser.error('Recipe requires either --install or --output')
        recipe, mismatches = matching_recipe(args.base, args.recipe)
        if recipe is None:
            details = '; '.join(f'{r.name}: {", ".join(t)}' for r, t in mismatches.items())
            if not args.if_matching:
                raise SystemExit(f'No repack recipe matches these tables ({details})')
            print('WARNING: shipping repack skipped; Tutorial_01 stays unpacked and loads more texture '
                  f'pages on a PSP. No recipe was measured for these tables ({details}).')
            return
        report = (install(args.base, recipe) if args.install else generate(args.base, recipe, args.output))
        print(f'Recipe: {recipe.name}')
    else:
        if args.install or not all((args.room, args.shared, args.output)):
            parser.error('Assembly requires --room, --shared and --output')
        report = assemble(args.base, args.room, args.shared, args.output)
    print(f"Shipping assets: {report['shared_owners']} owners, {report['shared_pages']} shared pages, "
          f"{report['room_pages']} Tutorial pages -> {args.base if args.install else args.output}")


if __name__ == '__main__':
    main()
