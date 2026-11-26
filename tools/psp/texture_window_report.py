"""Audit a possible local-UV swizzled texture view; never change assets/runtime.

The nearest 16-byte x 8-row origin preserves the sprite's swizzle phase. A
relocation must preserve local UVs AND declared dimensions for identical GE
arithmetic. This report checks the declared rectangle and a conservative full
stride footprint separately: accessed sprite taps alone cannot prove safety.
No window/residency policy is selected, and no hardware cost is measured here.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))


def validate_page(page_width, page_height, bits):
    if (bits not in (4, 8) or
            any(type(n) is not int for n in (page_width, page_height)) or
            any(n < 8 or n > 512 or n & (n-1)
                                  for n in (page_width, page_height)) or
            page_width*bits % 128):
        raise ValueError("Invalid PSP indexed page/window contract")


def window(page_width, page_height, bits, rect, padding=1, *, origin=None):
    """Describe a block origin, including potentially unsafe views."""
    validate_page(page_width, page_height, bits)
    if (not isinstance(rect, (tuple, list)) or len(rect) != 4 or
            any(type(n) is not int for n in rect) or
            type(padding) is not int or padding < 1):
        raise ValueError("Invalid sprite rectangle/padding")
    x, y, w, h = rect
    if (min(w, h) <= 0 or min(x, y) < padding or
            x+w+padding > page_width or y+h+padding > page_height):
        raise ValueError("Sprite padding escapes page")
    block_width = 128//bits
    ox = (x-padding)//block_width*block_width
    oy = (y-padding)//8*8
    if origin is not None:
        if (not isinstance(origin, (tuple, list)) or len(origin) != 2 or
                any(type(n) is not int for n in origin) or
                not 0 <= origin[0] <= ox or not 0 <= origin[1] <= oy or
                origin[0] % block_width or origin[1] % 8):
            raise ValueError("Invalid padded swizzle-block origin")
        ox, oy = origin
    width = max(block_width, 1 << (x+w+padding-ox-1).bit_length())
    height = max(8, 1 << (y+h+padding-oy-1).bit_length())
    row_bytes = page_width*bits//8
    offset = oy//8*row_bytes*8 + ox//block_width*128
    # Last declared block, including its entire eight rows. Retain original TBW.
    declared_end = offset + (height//8-1)*row_bytes*8 + width*bits//8*8
    stride_end = offset + row_bytes*height
    allocated = row_bytes*page_height
    declared_fits = (ox+width <= page_width and oy+height <= page_height and
                     declared_end <= allocated)
    stride_fits = stride_end <= allocated
    return dict(origin=[ox, oy], size=[width, height], stride=page_width, bits=bits,
                page_size=[page_width, page_height],
                offset=offset, declared_end=declared_end, stride_end=stride_end,
                allocated=allocated, local_uv=[x-ox, y-oy, x+w-ox, y+h-oy],
                declared_fits=declared_fits, full_stride_fits=stride_fits,
                conservative_safe=declared_fits and stride_fits)


def safe_window(page_width, page_height, bits, rect, padding=1):
    """Find the smallest declared view meeting both conservative bounds.

    Earlier origins may rescue an unsafe nearest view. The returned local UVs
    and dimensions are a packing constraint, not something to recompute after
    relocation: independently choosing another origin changes GE arithmetic.
    """
    nearest = window(page_width, page_height, bits, rect, padding)
    best = None
    best_key = None
    for oy in range(nearest['origin'][1], -1, -8):
        for ox in range(nearest['origin'][0], -1, -(128//bits)):
            view = window(page_width, page_height, bits, rect, padding,
                          origin=(ox, oy))
            key = (view['size'][0]*view['size'][1],
                   sum(view['local_uv'][:2]), -oy, -ox)
            if view['conservative_safe'] and (best_key is None or key < best_key):
                best, best_key = view, key
    # Origin zero with the full page declaration is always a safe fallback.
    assert best is not None
    return best


def relocated_window(source, page_width, page_height, bits, rect, padding=1):
    """Validate a placement against the source's canonical local GE view."""
    origin = (rect[0]-source['local_uv'][0], rect[1]-source['local_uv'][1])
    try:
        view = window(page_width, page_height, bits, rect, padding, origin=origin)
    except ValueError:
        return None
    if (not source['conservative_safe'] or not view['conservative_safe'] or
            view['local_uv'] != source['local_uv'] or
            view['size'] != source['size'] or view['stride'] != source['stride'] or
            view['bits'] != source['bits']):
        return None
    return view


def banks(manifest):
    result = {}
    seen = set()
    for line in manifest.read_text(encoding="utf-8").splitlines():
        name, path = line.split("\t")
        if not name or name in seen:
            raise ValueError("Duplicate manifest name")
        seen.add(name)
        path = Path(path)
        if path.suffix != ".bin":
            continue
        with path.open("rb") as file:
            magic = file.read(4)
        if magic in (b"PSR1", b"PSA1"):
            result[name] = path
    if not result:
        raise ValueError("No converted banks in manifest")
    return result


def views(table, safe_origins=False):
    # Keep the descriptor functions usable by the packer without an import cycle.
    import repack_pages
    count, records, _ = repack_pages.read_bank(table.read_bytes())
    pages = []
    for index in range(count):
        path = table.parent/f"page-{index}.hpt"
        with path.open("rb") as file:
            header = file.read(16)
        if len(header) != 16 or header[:4] != b"HPT1":
            raise ValueError("Invalid HPT header")
        w, h, mode, clut, swizzled, reserved, entries = struct.unpack_from("<HHBBBBI", header, 4)
        bits = {4: 4, 5: 8}.get(mode)
        if (bits is None or (clut, swizzled, reserved, entries) != (3, 1, 0, 1 << bits) or
                path.stat().st_size != 16+entries*4+w*h*bits//8):
            raise ValueError("Invalid swizzled indexed HPT")
        validate_page(w, h, bits)
        pages.append((w, h, bits))
    describe = safe_window if safe_origins else window
    return records, [describe(*pages[s[0]], s[1:5]) for s in records]


def report(before, after, safe_origins=False):
    left, right = banks(before), banks(after)
    if left.keys() != right.keys():
        raise ValueError("Bank names differ")
    rows, total = [], Counter()
    for name, path in left.items():
        a, av = views(path, safe_origins)
        b, bv = views(right[name], safe_origins)
        if len(a) != len(b) or any(x[3:] != y[3:] for x, y in zip(a, b)):
            raise ValueError("Sprite geometry/order differs")
        if safe_origins:
            # Retain the source's canonical phase even when a different bounded
            # origin would be nearer in the candidate. Failure stays visible in
            # the coordinate comparison; it must constrain a future packer.
            bv = [relocated_window(x, *y['page_size'], y['bits'], s[1:5]) or y
                  for x, y, s in zip(av, bv, b)]
        counts = Counter(sprites=len(a))
        for x, y in zip(av, bv):
            counts['before_declared_unsafe'] += not x['declared_fits']
            counts['after_declared_unsafe'] += not y['declared_fits']
            counts['before_full_stride_unsafe'] += not x['full_stride_fits']
            counts['after_full_stride_unsafe'] += not y['full_stride_fits']
            counts['before_conservative_unsafe'] += not x['conservative_safe']
            counts['after_conservative_unsafe'] += not y['conservative_safe']
            equal = (x['local_uv'] == y['local_uv'] and x['size'] == y['size'] and
                     x['bits'] == y['bits'] and x['stride'] == y['stride'])
            counts['equal_local_coordinates'] += equal
            counts['safe_equal_local_coordinates'] += (equal and x['conservative_safe'] and
                                                       y['conservative_safe'])
        rows.append(dict(bank=name, **counts))
        total.update(counts)
    return dict(banks=rows, totals=dict(total), safe_origins=safe_origins,
                note="Offline feasibility only; full-stride bound is conservative. No runtime policy or pixel parity acceptance.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', required=True, type=Path, help='Baseline assets.tsv')
    parser.add_argument('--after', required=True, type=Path, help='Candidate assets.tsv')
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--safe-origins', action='store_true',
                        help='Choose earlier bounded origins; compare local UVs and dimensions')
    args = parser.parse_args()
    result = report(args.before, args.after, args.safe_origins)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print(f"{len(result['banks'])} banks; {result['totals']}")


if __name__ == '__main__':
    main()
