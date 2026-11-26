"""Exact strip cover of the non-transparent support of large soft room sprites (fog, haze).

A bilinear sample is non-zero only within one texel of a texel whose RGBA4 alpha is
non-zero (the sprite's one-texel ring on the atlas page counts, because edge pixels
filter across the sprite border). The strips below are row bands whose column span is the
union of the 3x3-dilated support, so a draw restricted to them is bit-identical to the full
quad for Normal, Screen and Additive blends (all of them add nothing at alpha 0).

Output `<bank>.strips` (HKS1) is read by RoomVisualBank::loadTrims only when
sdmc:/3ds/HK3DS/fog-mesh.flag exists.
"""
import argparse
import json
import math
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))

MAGIC = b'HKS1'
MAX_SPRITES = 64
BANDS = 8
MIN_TEXELS = 1024
MAX_RATIO = 0.85


def dilated_spans(alpha, page_width, page_height, sprite):
    """Per sprite row: (first column, end column) of the 3x3-dilated alpha support, or None.

    Horizontal dilation only moves the extremes, so each row needs just its first and last
    non-zero texel (the ring of one texel around the sprite on the page included)."""
    _, x0, y0, width, height = sprite[:5]
    first, last = max(0, x0 - 1), min(page_width, x0 + width + 1)
    extent = []
    for row in range(-1, height + 1):
        py = y0 + row
        found = None
        if 0 <= py < page_height:
            data = bytes(alpha[py * page_width + first:py * page_width + last])
            lead = len(data) - len(data.lstrip(b'\x00'))
            if lead < len(data):
                found = (first + lead - x0, first + len(data.rstrip(b'\x00')) - x0)  # last non-zero col + 1
        extent.append(found)
    spans = []
    for row in range(height):
        near = [e for e in extent[row:row + 3] if e]
        if not near:
            spans.append(None)
            continue
        spans.append((max(0, min(e[0] for e in near) - 1), min(width, max(e[1] for e in near) + 1)))
    return spans


def best_bands(spans, bands=BANDS):
    """Row bands (row0, row1, col0, col1) of minimal total area covering every non-empty span."""
    rows = len(spans)
    inf = float('inf')
    best = [[inf] * (rows + 1) for _ in range(bands + 1)]
    back = [[None] * (rows + 1) for _ in range(bands + 1)]
    best[0][0] = 0
    for i in range(1, rows + 1):
        best[0][i] = 0 if spans[i - 1] is None and best[0][i - 1] == 0 else inf
    for k in range(1, bands + 1):
        for i in range(0, rows + 1):
            best[k][i] = best[k - 1][i]
            back[k][i] = ('same',)
            if i and spans[i - 1] is None and best[k][i - 1] < best[k][i]:
                best[k][i] = best[k][i - 1]
                back[k][i] = ('skip',)
            lo, hi = None, None
            for j in range(i - 1, -1, -1):
                if spans[j] is not None:
                    lo = spans[j][0] if lo is None else min(lo, spans[j][0])
                    hi = spans[j][1] if hi is None else max(hi, spans[j][1])
                if lo is None or spans[j] is None or best[k - 1][j] == inf:
                    continue
                cost = best[k - 1][j] + (i - j) * (hi - lo)
                if cost < best[k][i]:
                    best[k][i] = cost
                    back[k][i] = ('band', j, lo, hi)
    strips, k, i = [], bands, rows
    while i > 0 or k > 0:
        step = back[k][i] if k else None
        if k == 0 or step is None:
            break
        if step[0] == 'same':
            k -= 1
        elif step[0] == 'skip':
            i -= 1
        else:
            strips.append((step[1], i, step[2], step[3]))
            i, k = step[1], k - 1
    return sorted(strips), best[bands][rows]


def load_alpha(directory, fmt, cache, index):
    if index not in cache:
        from PIL import Image
        image = Image.open(Path(directory) / fmt.format(index)).convert('RGBA')
        cache[index] = (image.getchannel('A').tobytes(), image.width, image.height)
    return cache[index]


def sprite_strips(sprite, pages, fmt, directory, cache, bands=BANDS):
    alpha, width, height = load_alpha(directory, fmt, cache, sprite[0])
    spans = dilated_spans(alpha, width, height, sprite)
    block = max(1, math.ceil(len(spans) / 48))
    merged = []
    for start in range(0, len(spans), block):
        part = [x for x in spans[start:start + block] if x]
        merged.append((min(x[0] for x in part), max(x[1] for x in part)) if part else None)
    strips, _ = best_bands(merged, bands)
    strips = [(min(r0 * block, len(spans)), min(r1 * block, len(spans)), c0, c1) for r0, r1, c0, c1 in strips]
    # Block unions cover whole blocks; the area is of the rectangles actually drawn.
    return strips, sum((r1 - r0) * (c1 - c0) for r0, r1, c0, c1 in strips)


def build(bank_path, directory, fmt, bands=BANDS, limit=MAX_SPRITES):
    """Return (blob, stats). The sprites kept are those that save the most texel-area times world area."""
    from fog_composite import load_bank
    sprites, entries, _ = load_bank(bank_path)
    usage = {}
    for entry in entries:
        v = entry['values']
        if v[10] <= 0:
            continue
        sprite = sprites[entry['sprite']]
        usage[entry['sprite']] = usage.get(entry['sprite'], 0) + abs(v[0] * v[4] - v[1] * v[3]) * \
            (sprite[7] - sprite[5]) * (sprite[8] - sprite[6])
    cache, chosen = {}, []
    for index in sorted(usage):
        sprite = sprites[index]
        texels = sprite[3] * sprite[4]
        if texels < MIN_TEXELS or len(sprite[:5]) < 5:
            continue
        strips, area = sprite_strips(sprite, None, fmt, directory, cache, bands)
        ratio = area / texels
        if strips and ratio <= MAX_RATIO and len(strips) <= 64:
            chosen.append((usage[index] * (1 - ratio), index, ratio, strips))
    chosen = sorted(chosen, reverse=True)[:limit]
    chosen.sort(key=lambda item: item[1])
    blob = MAGIC + struct.pack('<II', len(sprites), len(chosen))
    for _, index, _, strips in chosen:
        blob += struct.pack('<II', index, len(strips))
        for row0, row1, col0, col1 in strips:
            blob += struct.pack('<4H', col0, col1, row0, row1)
    stats = [dict(sprite=index, size=sprites[index][3:5], ratio=round(ratio, 4), strips=len(strips))
             for _, index, ratio, strips in chosen]
    return blob, stats


def write(bank_path, directory, fmt, bands=BANDS):
    blob, stats = build(bank_path, directory, fmt, bands)
    target = bank_path.with_suffix('.strips')
    target.write_bytes(blob)
    return target, stats


def parse(blob):
    """Inverse of build() for tests: {sprite: [(col0, col1, row0, row1)]}."""
    assert blob[:4] == MAGIC
    count, sets = struct.unpack_from('<II', blob, 4)
    offset, result = 12, {}
    for _ in range(sets):
        sprite, n = struct.unpack_from('<II', blob, offset)
        offset += 8
        result[sprite] = [struct.unpack_from('<4H', blob, offset + 8 * i) for i in range(n)]
        offset += 8 * n
    assert offset == len(blob)
    return count, result


def report(bank_path, strips, cameras, names=None):
    """Draws that use strips and the projected fill they skip (model: texel ratio x projected px)."""
    from fog_composite import load_bank
    from fog_yield import corners, projected_pixels, target_for, FOG_NAME
    sprites, entries, _ = load_bank(bank_path)
    ratios = {}
    for index, sets in strips[1].items():
        sprite = sprites[index]
        ratios[index] = sum((c1 - c0) * (r1 - r0) for c0, c1, r0, r1 in sets) / (sprite[3] * sprite[4])
    rows = []
    for camera in cameras:
        row = dict(camera=camera, draws=0, quads=0, saved=0.0, fill=0.0, fog_saved=0.0, fog_fill=0.0)
        for entry in entries:
            v = entry['values']
            if v[10] <= 0:
                continue
            sprite = sprites[entry['sprite']]
            pixels = projected_pixels(corners(entry, sprite), camera, target_for(v[6]))
            fog = names is not None and FOG_NAME.match(names(entry['owner']) or '')
            if fog:
                row['fog_fill'] += pixels
            if entry['sprite'] not in ratios or pixels < 4096:
                continue
            saved = pixels * (1 - ratios[entry['sprite']])
            row['draws'] += 1
            row['quads'] += len(strips[1][entry['sprite']])
            row['saved'] += saved
            row['fill'] += pixels
            if fog:
                row['fog_saved'] += saved
        rows.append({k: round(x) if isinstance(x, float) else x for k, x in row.items()})
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bank', type=Path, required=True)
    parser.add_argument('--pages', type=Path, required=True, help='directory with the encoded atlas pages')
    parser.add_argument('--format', default='room-resident-page-{}-encoded.png')
    parser.add_argument('--bands', type=int, default=BANDS)
    parser.add_argument('--camera', action='append', default=[], help='x,y world camera, repeatable')
    parser.add_argument('--names', type=Path)
    parser.add_argument('--prefix')
    parser.add_argument('--no-write', action='store_true')
    args = parser.parse_args()
    if args.no_write:
        blob, stats = build(args.bank, args.pages, args.format, args.bands)
        target = None
    else:
        target, stats = write(args.bank, args.pages, args.format, args.bands)
        blob = target.read_bytes()
    out = dict(strips=str(target), sprites=stats)
    if args.camera:
        from fog_yield import load_names
        names = load_names(args.names, args.prefix) if args.names else None
        cams = [tuple(float(p) for p in text.split(',')) for text in args.camera]
        out['cameras'] = report(args.bank, parse(blob), cams, names)
    print(json.dumps(out, indent=1))


if __name__ == '__main__':
    main()
