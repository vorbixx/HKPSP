"""Lossless packing into fixed, explicitly budgeted texture pages."""
import hashlib
from PIL import Image


def _pack_order(sprites, sizes, sort_key, score_kind):
    pages = [Image.new('RGBA', size) for size in sizes]
    free_by_page = [[(0, 0, width, height)] for width, height in sizes]
    records = {}
    ordered = sorted(sprites, key=sort_key)
    for key, bitmap, geometry in ordered:
        width, height = bitmap.width + 2, bitmap.height + 2
        candidates = []
        for page_index, free in enumerate(free_by_page):
            for free_index, (x, y, fw, fh) in enumerate(free):
                if width > fw or height > fh:
                    continue
                remaining_w, remaining_h = fw-width, fh-height
                if score_kind == 'area':
                    score = (fw*fh-width*height, min(remaining_w, remaining_h),
                             max(remaining_w, remaining_h), page_index, y, x)
                elif score_kind == 'bottom':
                    score = (y + height, x, min(remaining_w, remaining_h),
                             max(remaining_w, remaining_h), page_index)
                else:
                    score = (min(remaining_w, remaining_h), max(remaining_w, remaining_h),
                             fw*fh-width*height, page_index, y, x)
                candidates.append((*score, free_index))
        if not candidates:
            return None
        choice = min(candidates)
        if score_kind == 'bottom':
            page_index, y, x, free_index = choice[4], choice[0] - height, choice[1], choice[5]
        else:
            page_index, y, x, free_index = choice[3], choice[4], choice[5], choice[6]
        px, py, pw, ph = x, y, width, height
        pages[page_index].paste(bitmap, (x+1, y+1))
        records[key] = (page_index, x+1, y+1, bitmap.width, bitmap.height, *geometry)

        next_free = []
        for fx, fy, fw, fh in free_by_page[page_index]:
            if px >= fx+fw or px+pw <= fx or py >= fy+fh or py+ph <= fy:
                next_free.append((fx, fy, fw, fh))
                continue
            if px > fx: next_free.append((fx, fy, px-fx, fh))
            if px+pw < fx+fw: next_free.append((px+pw, fy, fx+fw-(px+pw), fh))
            if py > fy: next_free.append((fx, fy, fw, py-fy))
            if py+ph < fy+fh: next_free.append((fx, py+ph, fw, fy+fh-(py+ph)))
        free_by_page[page_index] = [rect for i, rect in enumerate(next_free)
            if not any(i != j and rect[0] >= other[0] and rect[1] >= other[1]
                       and rect[0]+rect[2] <= other[0]+other[2]
                       and rect[1]+rect[3] <= other[1]+other[3]
                       for j, other in enumerate(next_free))]
    return records, pages


def pack_pages(sprites, sizes):
    """Try deterministic MaxRects heuristics while retaining every source pixel."""
    sprites = list(sprites)
    capacity = sum(width * height for width, height in sizes)
    unique = []
    by_digest = {}
    duplicate_of = {}
    originals = {key: (bitmap, geometry) for key, bitmap, geometry in sprites}
    for key, bitmap, geometry in sprites:
        pixels = bitmap.tobytes()
        signature = (bitmap.size, hashlib.sha256(pixels).digest())
        representative = next((prior_key for prior_key, prior_bitmap in by_digest.get(signature, ())
                              if prior_bitmap.tobytes() == pixels), None)
        if representative is None:
            unique.append((key, bitmap, geometry))
            by_digest.setdefault(signature, []).append((key, bitmap))
        else:
            duplicate_of[key] = representative
    sprites = unique
    padded_area = sum((sprite[1].width + 2) * (sprite[1].height + 2) for sprite in sprites)
    if padded_area > capacity:
        raise ValueError(f'Padded artwork exceeds the fixed texture page allocation: '
                         f'requires {padded_area} texels; fixed pages hold '
                         f'{capacity} texels ({len(sizes)} pages)')
    sort_keys = (
        lambda s: (-max(s[1].size), -s[1].width*s[1].height, s[0]),
        lambda s: (-s[1].width*s[1].height, -max(s[1].size), s[0]),
        lambda s: (-(s[1].width+s[1].height), -s[1].width*s[1].height, s[0]),
        lambda s: (-s[1].height, -s[1].width, s[0]),
        lambda s: (-s[1].width, -s[1].height, s[0]),
    )
    for sort_key in sort_keys:
        for score_kind in ('short', 'area', 'bottom'):
            packed = _pack_order(sprites, sizes, sort_key, score_kind)
            if packed is not None:
                records, pages = packed
                for key, representative in duplicate_of.items():
                    bitmap, geometry = originals[key]
                    records[key] = (*records[representative][:5], *geometry)
                return records, pages
    largest = max(sprites, key=lambda s: max(s[1].size))
    raise ValueError(f'Artwork exceeds the fixed texture page allocation near {largest[0]!r} '
                     f'({largest[1].width}x{largest[1].height}; {len(sizes)} pages)')
