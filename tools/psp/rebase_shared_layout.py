"""Rebase the measured Tutorial shared-page layout onto changed owner banks.

The shipped recipe (tools/psp/page-repack-tutorial.json) binds every shared owner table by SHA-256
and freezes each sprite's place on the shared pages. When a converter changes an owner's art (the
HUD bank on 2026-11-18: same 472 sprites and clips, larger rasters, two source pages), ship-repack
stops with "Shared table SHA256 mismatch" and Tutorial_01 shipped unpacked (214 page evictions in
600 combat frames on the PSP-3000).

This tool keeps every unchanged owner's sprites exactly where they were (byte-identical placement,
including the original-UV pins) and re-places only the changed owners' sprites: their used sprites
go into free space on the shared 'used' pages (a new 'used' page only if nothing fits), their unused
sprites are shelf-packed onto their own 'unused:<owner>' pages. A changed owner must keep its sprite
count and used/unused groups; anything else needs a fresh measurement, not a rebase.

  python -I tools/psp/rebase_shared_layout.py [--recipe tools/psp/page-repack-tutorial.json]
      [--base _private/converted-psp] [--write]

Without --write it only reports. Run it under Python 3.12 (boss-python PIL), then
`tools/psp/regen-assets.ps1 -Only ship-repack`, which re-verifies every sprite byte for byte.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import repack_pages as pack  # noqa: E402
import shared_page_repack as shared  # noqa: E402

PAD = pack.room.PAD


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def overlaps(a, b) -> bool:
    return a[0] < b[0]+b[2] and b[0] < a[0]+a[2] and a[1] < b[1]+b[3] and b[1] < a[1]+a[3]


def place(rects: list, w: int, h: int, size: int):
    """Bottom-left first fit of a padded w x h rectangle among padded rects on one page."""
    pw, ph = w+2*PAD, h+2*PAD
    xs = sorted({0} | {r[0]+r[2] for r in rects})
    ys = sorted({0} | {r[1]+r[3] for r in rects})
    for y in ys:
        if y+ph > size:
            break
        for x in xs:
            if x+pw > size:
                break
            cand = (x, y, pw, ph)
            if not any(overlaps(cand, r) for r in rects):
                rects.append(cand)
                return x+PAD, y+PAD
    return None


def rebase(recipe: dict, base: Path) -> tuple[dict, dict]:
    Image = pack.room.import_pil()
    profile = recipe['shared']
    layout = profile['layout']
    old_records = [tuple(s) for s in layout['sprite_records']]
    pages, records, groups, owners, changed = [], [], [], [], []
    for entry in profile['banks']:
        table = base/entry['table']
        raw = table.read_bytes()
        count, recs, _ = pack.read_bank(raw)
        if len(recs) != len(entry['groups']):
            raise ValueError(f"{entry['name']}: sprite count changed ({len(entry['groups'])} -> {len(recs)}); "
                             "re-measure the pool instead of rebasing")
        page_shas = [sha(table.parent/f'page-{i}.hpt') for i in range(count)]
        if sha(table) != entry['table_sha256'] or page_shas != entry['page_sha256']:
            changed.append(entry['name'])
        records.extend((s[0]+len(pages), *s[1:]) for s in recs)
        groups.extend(entry['groups'])
        owners.extend([entry['name']]*len(recs))
        pages.extend(pack.read_pages(Image, table.parent, count))
    if len(records) != len(old_records):
        raise ValueError('Shared sprite count changed')
    size = pages[0].size[0]
    page_groups = list(layout['page_groups'])
    keep = [owners[i] not in changed for i in range(len(records))]
    for i in layout.get('original_uv_sprites', []):
        if not keep[i]:
            raise ValueError(f'Original-UV pinned sprite {i} belongs to a changed owner')
    # Unchanged owners: same page, x, y; their geometry must not have moved.
    new_records = [None]*len(records)
    rects = {p: [] for p in range(len(page_groups))}
    for i, old in enumerate(old_records):
        if keep[i]:
            if old[3:] != tuple(records[i][3:]):
                raise ValueError(f'Unchanged owner sprite {i} geometry differs')
            new_records[i] = old
            p, x, y, w, h = old[:5]
            rects[p].append((x-PAD, y-PAD, w+2*PAD, h+2*PAD))
    report = dict(changed=changed, placed_used=0, new_used_pages=0, unused_pages={})
    for name in changed:
        ids = [i for i in range(len(records)) if owners[i] == name]
        # Its own unused pages are rebuilt from scratch.
        tag = f'unused:{name}'
        own_pages = [p for p, g in enumerate(page_groups) if g == tag]
        for p in own_pages:
            rects[p] = []
        used = sorted((i for i in ids if groups[i] == 'used'),
                      key=lambda i: (-records[i][4], -records[i][3], i))
        used_pages = [p for p, g in enumerate(page_groups) if g == 'used']
        for i in used:
            w, h = records[i][3:5]
            for p in used_pages:
                spot = place(rects[p], w, h, size)
                if spot:
                    break
            else:
                page_groups.append('used')
                p = len(page_groups)-1
                rects[p] = []
                used_pages.append(p)
                report['new_used_pages'] += 1
                spot = place(rects[p], w, h, size)
            new_records[i] = (p, *spot, *records[i][3:])
            report['placed_used'] += 1
        unused = [i for i in ids if groups[i] == tag]
        if unused:
            places, count = pack.room.pack([records[i][3:5] for i in unused], page=size)
            while len(own_pages) < count:
                page_groups.append(tag)
                own_pages.append(len(page_groups)-1)
            for i, (p, x, y) in zip(unused, places):
                new_records[i] = (own_pages[p], x, y, *records[i][3:])
            if len(own_pages) > count:
                raise ValueError(f'{name}: unused sprites now need fewer pages; re-measure instead')
            report['unused_pages'][name] = count
        other = [i for i in ids if groups[i] not in ('used', tag)]
        if other:
            raise ValueError(f'{name}: unexpected groups {sorted({groups[i] for i in other})}')
    # Serialize exactly as restore_mixed_layout does, then verify every padded sprite.
    count = len(page_groups)
    indices = [bytearray(size*size) for _ in range(count)]
    for old, new in zip(records, new_records):
        p, x, y, w, h = new[:5]
        source, ox, oy = old[:3]
        crop = pages[source].crop((ox-PAD, oy-PAD, ox+w+PAD, oy+h+PAD)).tobytes()
        stride = w+2*PAD
        for row in range(h+2*PAD):
            start = (y-PAD+row)*size+x-PAD
            indices[p][start:start+stride] = crop[row*stride:(row+1)*stride]
    source_cluts = [pack.palette_bytes(p) for p in pages]
    palettes = list(dict.fromkeys(source_cluts))
    clut_of = {c: i for i, c in enumerate(palettes)}
    plan = dict(size=(size, size), indices=[bytes(p) for p in indices], palettes=palettes,
                sprites=new_records, cluts=[clut_of[source_cluts[s[0]]] for s in records],
                groups=page_groups, algorithm=layout['profile']['algorithm'])
    pack.verify_mixed(pages, records, groups, plan)
    out = copy.deepcopy(recipe)
    banks = out['shared']['banks']
    for entry in banks:
        if entry['name'] in changed:
            table = base/entry['table']
            count_pages = pack.read_bank(table.read_bytes())[0]
            entry['table_sha256'] = sha(table)
            entry['page_sha256'] = [sha(table.parent/f'page-{i}.hpt') for i in range(count_pages)]
    new_layout = dict(layout)
    new_layout.update(page_groups=page_groups, after_pages=count, sprite_records=[list(s) for s in new_records],
                      sprite_cluts=plan['cluts'],
                      index_sha256=[hashlib.sha256(p).hexdigest() for p in plan['indices']],
                      clut_sha256=[hashlib.sha256(p).hexdigest() for p in palettes])
    without = {k: v for k, v in out['shared'].items() if k != 'layout'}
    new_layout['profile'] = shared.layout_profile(without, groups)
    out['shared']['layout'] = new_layout
    report.update(pages_before=len(layout['page_groups']), pages_after=count,
                  used_pages=page_groups.count('used'))
    return out, report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--recipe', type=Path, default=ROOT/'tools/psp/page-repack-tutorial.json')
    parser.add_argument('--base', type=Path, default=ROOT/'_private/converted-psp')
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args()
    recipe = json.loads(args.recipe.read_text(encoding='utf-8'))
    out, report = rebase(recipe, args.base)
    print(json.dumps(report))
    if not report['changed']:
        print('recipe already matches the converted owners')
        return 0
    if args.write:
        args.recipe.write_text(json.dumps(out, indent=2)+'\n', encoding='utf-8', newline='\n')
        print(f'wrote {args.recipe}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
