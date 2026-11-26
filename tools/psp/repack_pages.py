"""Losslessly repack converted PSR1/PSA1 sprites using explicit owner/use groups.

Quantization must happen BEFORE this pass: changing the input to a page's
quantizer changes its texels. The shipping HPT1 format has one CLUT per page.
Profiles bind sprite group names to the SHA256 of the baseline table.
Optional canonical_views preserves the source's local GE coordinates, declared
dimensions and stride; this remains an offline candidate until runtime parity.

Profile JSON: {"table_sha256": "...", "groups": ["hero/locomotion", ...]}.
Run under validation-lock.ps1 -Write. Output must be a new directory. Every
sprite, including unused sprites, needs a group. This pass does not infer use
from page-level traces or choose a default policy; profiles are explicit.
"""
from __future__ import annotations

import argparse
import hashlib
import heapq
import json
import math
from functools import lru_cache
from pathlib import Path
import re
import shutil
import struct
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import transcode_room as room
import texture_window_report as windows


def palette_bytes(image):
    if image.mode != "P":
        raise ValueError("Repacking requires indexed pages")
    palette = bytes(image.getpalette(rawmode="RGBA") or [])
    if len(palette) != 1024:
        raise ValueError("Repacking requires a 256-entry RGBA CLUT")
    return palette


def pack_fixed(pages, sprites, groups):
    """Offline multi-CLUT plan: preserve absolute UVs and the full page view.

    A group owns contiguous pages; padded overlaps may share storage only if
    every index agrees. CLUTs are independent of page IDs. This is deliberately
    separate from convert(): no shipping format or runtime policy is selected.
    """
    if not pages or any(p.size != pages[0].size for p in pages):
        raise ValueError("Fixed packing requires equal source page dimensions")
    if len(groups) != len(sprites) or any(not isinstance(g, str) or not g for g in groups):
        raise ValueError("Expected one nonempty group name per sprite")
    width, height = pages[0].size
    source = [p.tobytes() for p in pages]
    source_cluts = [palette_bytes(p) for p in pages]
    palettes = list(dict.fromkeys(source_cluts))
    cluts = {p: i for i, p in enumerate(palettes)}
    buckets, rectangles = {}, []
    for i, (s, group) in enumerate(zip(sprites, groups)):
        p, x, y, w, h = s[:5]
        pad = room.PAD
        if (not 0 <= p < len(pages) or min(w, h) <= 0 or min(x, y) < pad or
                x+w+pad > width or y+h+pad > height):
            raise ValueError(f"Invalid padded sprite {i}")
        rectangles.append((x-pad, y-pad, x+w+pad, y+h+pad))
        buckets.setdefault(group, []).append(i)

    output, records, metadata = [], list(sprites), []
    for group, ids in buckets.items():
        occupied = []
        first = len(output)
        for i in sorted(ids, key=lambda i: (-sprites[i][3]*sprites[i][4], i)):
            p = sprites[i][0]
            rect = rectangles[i]
            for slot in range(len(occupied)+1):
                if slot == len(occupied):
                    occupied.append([])
                    output.append(bytearray(width*height))
                    metadata.append(group)
                compatible = True
                for old_page, old_rect in occupied[slot]:
                    if old_page == p:
                        continue
                    left, top = max(rect[0], old_rect[0]), max(rect[1], old_rect[1])
                    right, bottom = min(rect[2], old_rect[2]), min(rect[3], old_rect[3])
                    if left >= right or top >= bottom:
                        continue
                    if any(source[p][r*width+left:r*width+right] !=
                           source[old_page][r*width+left:r*width+right]
                           for r in range(top, bottom)):
                        compatible = False
                        break
                if not compatible:
                    continue
                occupied[slot].append((p, rect))
                destination = first+slot
                records[i] = (destination, *sprites[i][1:])
                for row in range(rect[1], rect[3]):
                    a, b = row*width+rect[0], row*width+rect[2]
                    output[destination][a:b] = source[p][a:b]
                break
        # First-fit may fragment a source page. The unchanged source-page
        # partition is a valid, often smaller upper bound within this group.
        source_ids = list(dict.fromkeys(sprites[i][0] for i in ids))
        if len(occupied) > len(source_ids):
            del output[first:]
            del metadata[first:]
            source_slots = {p: first+j for j, p in enumerate(source_ids)}
            output.extend(bytearray(width*height) for _ in source_ids)
            metadata.extend(group for _ in source_ids)
            for i in ids:
                p = sprites[i][0]
                destination = source_slots[p]
                records[i] = (destination, *sprites[i][1:])
                rect = rectangles[i]
                for row in range(rect[1], rect[3]):
                    a, b = row*width+rect[0], row*width+rect[2]
                    output[destination][a:b] = source[p][a:b]
    plan = dict(size=(width, height), indices=[bytes(p) for p in output],
                palettes=palettes, sprites=records,
                cluts=[cluts[source_cluts[s[0]]] for s in sprites], groups=metadata)
    verify_fixed(pages, sprites, plan)
    return plan


def verify_fixed(before, sprites, plan):
    """Guard every retained sprite, its filter border, absolute view and CLUT."""
    if (len(plan['sprites']) != len(sprites) or len(plan['cluts']) != len(sprites) or
            any(p.size != tuple(plan['size']) for p in before)):
        raise ValueError("Fixed sprite count or texture view changed")
    width, height = plan['size']
    if any(len(p) != width*height for p in plan['indices']):
        raise ValueError("Fixed texture payload size changed")
    for i, (old, new) in enumerate(zip(sprites, plan['sprites'])):
        p, x, y, w, h = old[:5]
        if (old[1:] != new[1:] or not 0 <= new[0] < len(plan['indices']) or
                not 0 <= plan['cluts'][i] < len(plan['palettes']) or
                palette_bytes(before[p]) != plan['palettes'][plan['cluts'][i]]):
            raise ValueError(f"Sprite {i} fixed view or CLUT changed")
        expected = before[p].crop((x-room.PAD, y-room.PAD,
                                   x+w+room.PAD, y+h+room.PAD)).tobytes()
        actual = b''.join(plan['indices'][new[0]][row*width+x-room.PAD:
                         row*width+x+w+room.PAD] for row in range(y-room.PAD, y+h+room.PAD))
        if expected != actual:
            raise ValueError(f"Sprite {i} fixed padded indices changed")


def window_places(sprites, ids, masks, size):
    """Deterministic MaxRects, ordered by first grown-window use then area.

    Prefer an existing page with the fewest additional window memberships.
    Maximal free rectangles may overlap each other; every placement subtracts
    its entire padded area from every free rectangle before the next sprite.
    """
    places, free, page_masks = {}, [], []
    order = sorted(ids, key=lambda i: (
        (masks[i] & -masks[i]).bit_length() if masks[i] else float('inf'),
        -(sprites[i][3]+2*room.PAD)*(sprites[i][4]+2*room.PAD), i))
    for i in order:
        width, height = (v+2*room.PAD for v in sprites[i][3:5])
        best = None
        for page, rectangles in enumerate(free):
            for x, y, w, h in rectangles:
                if width > w or height > h:
                    continue
                cost = ((masks[i] & ~page_masks[page]).bit_count(),
                        min(w-width, h-height), max(w-width, h-height), page, x, y)
                if best is None or cost < best:
                    best = cost
        if best is None:
            page, x, y = len(free), 0, 0
            free.append([(0, 0, size, size)])
            page_masks.append(0)
        else:
            _, _, _, page, x, y = best
        pending = []
        for a, b, w, h in free[page]:
            right, bottom = a+w, b+h
            if x+width <= a or right <= x or y+height <= b or bottom <= y:
                pending.append((a, b, w, h))
                continue
            if x > a:
                pending.append((a, b, x-a, h))
            if x+width < right:
                pending.append((x+width, b, right-x-width, h))
            if y > b:
                pending.append((a, b, w, y-b))
            if y+height < bottom:
                pending.append((a, y+height, w, bottom-y-height))
        pending = list(dict.fromkeys(pending))
        free[page] = [r for j, r in enumerate(pending) if not any(
            k != j and s[0] <= r[0] and s[1] <= r[1] and
            s[0]+s[2] >= r[0]+r[2] and s[1]+s[3] >= r[1]+r[3]
            for k, s in enumerate(pending))]
        page_masks[page] |= masks[i]
        places[i] = (page, x+room.PAD, y+room.PAD)
    return [places[i] for i in ids], len(free)


def cluster_places(sprites, ids, masks, size):
    """Merge fitting clusters by shared window memberships, not first use.

    Each accepted merge reduces page memberships in its common windows. Test
    actual padded 2D fit before merging; area alone is only an early rejection.
    This offline alternative may use more package pages for better locality.
    """
    zeros = [0]*len(sprites)
    clusters = {i: ((i,), masks[i], (sprites[i][3]+2*room.PAD)*(sprites[i][4]+2*room.PAD),
                    [(0, room.PAD, room.PAD)]) for i in ids}
    heap = []

    def push(i, j):
        _, am, aa, _ = clusters[i]
        _, bm, ba, _ = clusters[j]
        if aa+ba <= size*size:
            heapq.heappush(heap, (-(am & bm).bit_count(), -(aa+ba), i, j))

    for n, i in enumerate(ids):
        for j in ids[n+1:]:
            push(i, j)
    serial = len(sprites)
    while heap:
        _, _, i, j = heapq.heappop(heap)
        if i not in clusters or j not in clusters:
            continue
        a, am, aa, _ = clusters[i]
        b, bm, ba, _ = clusters[j]
        merged = tuple(sorted(a+b))
        places, count = window_places(sprites, merged, zeros, size)
        if count != 1:
            continue
        del clusters[i], clusters[j]
        clusters[serial] = (merged, am | bm, aa+ba, places)
        for k in clusters:
            if k != serial:
                push(k, serial)
        serial += 1
    output = {}
    for page, (members, _, _, places) in enumerate(clusters.values()):
        for i, (_, x, y) in zip(members, places):
            output[i] = page, x, y
    return [output[i] for i in ids], len(clusters)


def refine_window_places(sprites, layouts, masks, size):
    """Move a worst-window subset into a fitting page of the same group.

    Minimize the descending histogram of window counts over the whole bank,
    including all owner/use groups. Every accepted transfer strictly improves
    it; bounded descent is a sufficient layout, never a minimum proof.
    """
    bins, page_groups, positions = [], [], {}
    for group, ids, places, count in layouts:
        first = len(bins)
        bins.extend(set() for _ in range(count))
        page_groups.extend(group for _ in range(count))
        for i, (page, x, y) in zip(ids, places):
            bins[first+page].add(i)
            positions[i] = first+page, x, y
    areas = [(s[3]+2*room.PAD)*(s[4]+2*room.PAD) for s in sprites]

    def union(ids):
        result = 0
        for i in ids:
            result |= masks[i]
        return result

    page_masks = list(map(union, bins))
    windows = [{i for i, mask in enumerate(masks) if (mask >> f) & 1}
               for f in range(max(masks, default=0).bit_length())]
    counts = [sum((m >> f) & 1 for m in page_masks) for f in range(len(windows))]

    def score(values):
        histogram = [0]*(len(bins)+1)
        for n in values:
            histogram[n] += 1
        return tuple(reversed(histogram))

    current = score(counts)
    for _ in range(64):
        attempted, winner = set(), None
        for f in sorted(range(len(windows)), key=lambda f: (-counts[f], f))[:24]:
            for source, ids in enumerate(bins):
                take = frozenset(ids & windows[f])
                if not take or (source, take) in attempted:
                    continue
                attempted.add((source, take))
                remain = ids-take
                source_mask = union(remain)
                for target, other in enumerate(bins):
                    if target == source or not other or page_groups[source] != page_groups[target]:
                        continue
                    merged = other | take
                    if sum(areas[i] for i in merged) > size*size:
                        continue
                    target_mask = union(merged)
                    proposed = [n+((source_mask >> k) & 1)+((target_mask >> k) & 1)-
                                ((page_masks[source] >> k) & 1)-((page_masks[target] >> k) & 1)
                                for k, n in enumerate(counts)]
                    candidate = score(proposed)
                    if candidate >= current or (winner is not None and candidate >= winner[0]):
                        continue
                    ordered = sorted(merged)
                    places, count = window_places(sprites, ordered, [0]*len(sprites), size)
                    if count == 1:
                        winner = (candidate, source, target, remain, merged, source_mask,
                                  target_mask, proposed, ordered, places)
        if winner is None:
            break
        (current, source, target, remaining, merged, source_mask,
         target_mask, counts, ordered, places) = winner
        bins[source], bins[target] = remaining, merged
        page_masks[source], page_masks[target] = source_mask, target_mask
        for i, (_, x, y) in zip(ordered, places):
            positions[i] = target, x, y
    result = []
    for group, ids, _, _ in layouts:
        pages = sorted({positions[i][0] for i in ids})
        remap = {page: i for i, page in enumerate(pages)}
        result.append((group, ids, [(remap[positions[i][0]], *positions[i][1:]) for i in ids], len(pages)))
    return result


def exchange_window_places(sprites, layouts, masks, size):
    """Bounded same-group exchanges, with a third page for displaced sprites.

    A source's entire witness subset moves into an already visible target.
    Displaced non-witness sprites return to the source or a third page. Score
    every original window before exact padded MaxRects fitting; never accept
    an intermediate move on its own. This is strictly an offline search.
    """
    bins, page_groups, positions = [], [], {}
    for group, ids, places, count in layouts:
        first = len(bins)
        bins.extend(frozenset() for _ in range(count))
        page_groups.extend(group for _ in range(count))
        for i, (page, x, y) in zip(ids, places):
            bins[first+page] |= {i}
            positions[i] = first+page, x, y
    areas = [(s[3]+2*room.PAD)*(s[4]+2*room.PAD) for s in sprites]
    window_count = max(masks, default=0).bit_length()
    windows = [frozenset(i for i, m in enumerate(masks) if (m >> f) & 1)
               for f in range(window_count)]

    @lru_cache(maxsize=65536)
    def union(ids):
        result = 0
        for i in ids:
            result |= masks[i]
        return result

    def score(counts):
        histogram = [0]*(len(bins)+1)
        for n in counts:
            histogram[n] += 1
        return tuple(reversed(histogram))

    page_masks = list(map(union, bins))
    counts = [sum((m >> f) & 1 for m in page_masks) for f in range(window_count)]
    current = score(counts)
    zeros = [0]*len(sprites)
    @lru_cache(maxsize=16384)
    def fit(members):
        ordered = sorted(members)
        places, count = window_places(sprites, ordered, zeros, size)
        return list(zip(ordered, places)) if count <= 1 else None

    for _ in range(64):
        levels = [0]*(len(bins)+1)
        for f, n in enumerate(counts):
            levels[n] |= 1 << f
        histogram = list(reversed(current))

        def updated(changed):
            # Disjoint bitsets of windows with each net membership delta.
            deltas = {0: (1 << window_count)-1}
            for page, ids in changed.items():
                proposed = union(ids)
                gain, loss = proposed & ~page_masks[page], page_masks[page] & ~proposed
                stay = ~(gain | loss)
                pending = {}
                for delta, bits in deltas.items():
                    for d, b in ((delta, bits & stay), (delta+1, bits & gain),
                                 (delta-1, bits & loss)):
                        if b:
                            pending[d] = pending.get(d, 0) | b
                deltas = pending
            hist = histogram.copy()
            for delta, bits in deltas.items():
                if delta:
                    for n, level in enumerate(levels):
                        count = (bits & level).bit_count()
                        if count:
                            hist[n] -= count
                            hist[n+delta] += count
            return tuple(reversed(hist))

        attempted, winner = set(), None
        for f in sorted(range(window_count), key=lambda f: (-counts[f], f))[:24]:
            witness = windows[f]
            for source, ids in enumerate(bins):
                take = ids & witness
                if not take or (source, take) in attempted:
                    continue
                attempted.add((source, take))
                for target, other in enumerate(bins):
                    if (target == source or not other & witness or
                            page_groups[source] != page_groups[target]):
                        continue
                    absent = other-witness
                    same_masks = {}
                    for i in sorted(absent):
                        same_masks.setdefault(masks[i], set()).add(i)
                    choices = {frozenset(), absent, *(frozenset([i]) for i in absent),
                               *(frozenset(v) for v in same_masks.values())}
                    for give in sorted(choices, key=lambda v: (len(v), tuple(sorted(v)))):
                        merged = (other-give) | take
                        if sum(areas[i] for i in merged) > size*size:
                            continue
                        receivers = [source] if not give else range(len(bins))
                        for receiver in receivers:
                            if receiver == target or page_groups[receiver] != page_groups[source]:
                                continue
                            changed = {source: ids-take, target: merged}
                            changed[receiver] = changed.get(receiver, bins[receiver]) | give
                            if any(sum(areas[i] for i in v) > size*size for v in changed.values()):
                                continue
                            candidate = updated(changed)
                            if candidate >= current or (winner is not None and candidate >= winner[0]):
                                continue
                            fitted = {}
                            for page, members in changed.items():
                                if page == source and receiver != source:
                                    continue  # Removal preserves the existing padded positions.
                                entries = fit(members)
                                if entries is None:
                                    break
                                fitted[page] = entries
                            else:
                                proposed_masks = page_masks.copy()
                                for page, members in changed.items():
                                    proposed_masks[page] = union(members)
                                proposed = [sum((m >> k) & 1 for m in proposed_masks)
                                            for k in range(window_count)]
                                assert candidate == score(proposed)
                                winner = candidate, changed, fitted, proposed_masks, proposed
        if winner is None:
            break
        current, changed, fitted, page_masks, counts = winner
        for page, members in changed.items():
            bins[page] = members
        for page, entries in fitted.items():
            for i, (_, x, y) in entries:
                positions[i] = page, x, y
    result = []
    for group, ids, _, _ in layouts:
        pages = sorted({positions[i][0] for i in ids})
        remap = {page: i for i, page in enumerate(pages)}
        result.append((group, ids, [(remap[positions[i][0]], *positions[i][1:]) for i in ids], len(pages)))
    return result


def pack_mixed(pages, sprites, groups, footprints=None, *, optimize_windows=False):
    """Relocate padded indices by owner/use, independently of the exact CLUT.

    This is an offline plan. Pages have the original dimensions and stride;
    each sprite references its original palette through a separate CLUT table.
    No sprite rotation, resampling, re-quantization or geometry change occurs.
    """
    if not pages or any(p.size != pages[0].size for p in pages):
        raise ValueError("Mixed packing requires equal source page dimensions")
    width, height = pages[0].size
    if width != height:
        raise ValueError("Mixed packing requires square pages")
    if len(groups) != len(sprites) or any(not isinstance(g, str) or not g for g in groups):
        raise ValueError("Expected one nonempty group name per sprite")
    masks = None
    if footprints is not None:
        masks = [0]*len(sprites)
        for frame, ids in enumerate(dict.fromkeys(frozenset(w) for w in footprints)):
            for i in ids:
                if not isinstance(i, int) or not 0 <= i < len(sprites):
                    raise ValueError("Invalid sprite in camera footprint")
                masks[i] |= 1 << frame
    source_cluts = [palette_bytes(p) for p in pages]
    palettes = list(dict.fromkeys(source_cluts))
    cluts = {p: i for i, p in enumerate(palettes)}
    buckets = {}
    for i, (s, group) in enumerate(zip(sprites, groups)):
        p, x, y, w, h = s[:5]
        if (not 0 <= p < len(pages) or min(w, h) <= 0 or min(x, y) < room.PAD or
                x+w+room.PAD > width or y+h+room.PAD > height):
            raise ValueError(f"Invalid padded sprite {i}")
        buckets.setdefault(group, []).append(i)
    layouts = []
    for group, ids in buckets.items():
        if masks is None:
            places, count = room.pack([sprites[i][3:5] for i in ids], page=width)
        else:
            places, count = window_places(sprites, ids, masks, width)
        layouts.append((group, ids, places, count))
    algorithm = 'first-window-area-maxrects' if masks is not None else 'shelf'
    if optimize_windows and masks is not None:
        candidates = []
        for group, ids, places, count in layouts:
            # A single page is already optimal; equal masks cannot improve
            # locality by clustering. Keep their compact original packing.
            if count > 1 and len({masks[i] for i in ids}) > 1:
                places, count = cluster_places(sprites, ids, masks, width)
            candidates.append((group, ids, places, count))

        def score(layout):
            page_masks = []
            for _, ids, places, count in layout:
                combined = [0]*count
                for i, (page, _, _) in zip(ids, places):
                    combined[page] |= masks[i]
                page_masks.extend(combined)
            counts = [sum((m >> frame) & 1 for m in page_masks)
                      for frame in range(max(masks, default=0).bit_length())]
            return tuple(sorted(counts, reverse=True)), len(page_masks)

        candidates = refine_window_places(sprites, candidates, masks, width)
        if score(candidates) < score(layouts):
            layouts = candidates
            algorithm = 'shared-window-gain-transfer-maxrects'
        exchanged = exchange_window_places(sprites, layouts, masks, width)
        if score(exchanged) < score(layouts):
            layouts = exchanged
            algorithm = 'shared-window-gain-exchange-maxrects'
    output, records, metadata = [], list(sprites), []
    for group, ids, places, count in layouts:
        first = len(output)
        output.extend(bytearray(width*height) for _ in range(count))
        metadata.extend(group for _ in range(count))
        for i, (p, x, y) in zip(ids, places):
            old, ox, oy, w, h = sprites[i][:5]
            pad = room.PAD
            crop = pages[old].crop((ox-pad, oy-pad, ox+w+pad, oy+h+pad)).tobytes()
            stride = w+2*pad
            for row in range(h+2*pad):
                start = (y-pad+row)*width+x-pad
                output[first+p][start:start+stride] = crop[row*stride:(row+1)*stride]
            records[i] = (first+p, x, y, *sprites[i][3:])
    plan = dict(size=(width, height), indices=[bytes(p) for p in output],
                palettes=palettes, sprites=records,
                cluts=[cluts[source_cluts[s[0]]] for s in sprites], groups=metadata,
                algorithm=algorithm)
    verify_mixed(pages, sprites, groups, plan)
    return plan


def verify_mixed(before, sprites, groups, plan):
    """Audit the serialized-plan contract for every sprite, including unused ones."""
    if (len(plan['sprites']) != len(sprites) or len(plan['cluts']) != len(sprites) or
            len(groups) != len(sprites) or len(plan['groups']) != len(plan['indices']) or
            any(p.size != tuple(plan['size']) for p in before)):
        raise ValueError("Mixed sprite count or texture view changed")
    width, height = plan['size']
    if any(len(p) != width*height for p in plan['indices']):
        raise ValueError("Mixed texture payload size changed")
    for i, (old, new) in enumerate(zip(sprites, plan['sprites'])):
        p, x, y, w, h = new[:5]
        if (old[3:] != new[3:] or not 0 <= p < len(plan['indices']) or
                min(x, y) < room.PAD or x+w+room.PAD > width or y+h+room.PAD > height or
                not 0 <= plan['cluts'][i] < len(plan['palettes']) or
                palette_bytes(before[old[0]]) != plan['palettes'][plan['cluts'][i]] or
                plan['groups'][p] != groups[i]):
            raise ValueError(f"Sprite {i} mixed geometry, owner/use or CLUT changed")
        _, ox, oy, w, h = old[:5]
        pad = room.PAD
        expected = before[old[0]].crop((ox-pad, oy-pad, ox+w+pad, oy+h+pad)).tobytes()
        actual = b''.join(plan['indices'][p][r*width+x-pad:r*width+x+w+pad]
                          for r in range(y-pad, y+h+pad))
        if expected != actual:
            raise ValueError(f"Sprite {i} mixed padded indices changed")


def pack_canonical(pages, sprites, ids, page_size):
    """First-fit padded rectangles on each source canonical view's origin grid.

    Occupancy covers copied texels, not unused parts of a declared texture.
    A view may overlap another view, but their padded sprite texels may not.
    Every admitted view retains both conservative address bounds.
    """
    views = {}
    for i in ids:
        page = pages[sprites[i][0]]
        if page.size != (page_size, page_size):
            raise ValueError("Canonical packing requires unchanged page dimensions/stride")
        views[i] = windows.safe_window(*page.size, 8, sprites[i][1:5], room.PAD)
    occupied, placements = [], {}
    order = sorted(ids, key=lambda i: (-(sprites[i][3]+2*room.PAD)*
                                      (sprites[i][4]+2*room.PAD), i))
    for i in order:
        source = views[i]
        w, h = sprites[i][3:5]
        local_x, local_y = source['local_uv'][:2]
        width, height = source['size']
        placed = False
        for p in range(len(occupied)+1):
            if p == len(occupied):
                occupied.append([0]*page_size)
            for oy in range(0, page_size-height+1, 8):
                for ox in range(0, page_size-width+1, 16):
                    # Same TBW: the full-height span also includes the origin's
                    # horizontal swizzle-block offset.
                    if oy*page_size+ox*8+page_size*height > page_size*page_size:
                        continue
                    x, y = ox+local_x, oy+local_y
                    mask = ((1 << (w+2*room.PAD))-1) << (x-room.PAD)
                    rows = range(y-room.PAD, y+h+room.PAD)
                    if any(occupied[p][row] & mask for row in rows):
                        continue
                    if windows.relocated_window(source, page_size, page_size, 8,
                                                (x, y, w, h), room.PAD) is None:
                        raise ValueError(f"Canonical placement invariant failed for sprite {i}")
                    for row in rows:
                        occupied[p][row] |= mask
                    placements[i] = (p, x, y)
                    placed = True
                    break
                if placed:
                    break
            if placed:
                break
        if not placed:
            raise ValueError(f"No canonical placement for sprite {i}")
    return [placements[i] for i in ids], len(occupied)


def repack(Image, pages, sprites, groups, page_size=room.PAGE, *, canonical_views=False):
    """Keep sprite IDs, dimensions, geometry, CLUT and padded indices exact.

    Group order is first occurrence in the sprite table; each group's pages
    are contiguous. Within a group, separate incompatible baseline palettes.
    All sprites, including unreferenced frames, must have an explicit group.
    """
    if len(groups) != len(sprites) or any(not isinstance(g, str) or not g for g in groups):
        raise ValueError("Expected one nonempty group name per sprite")
    palettes = [palette_bytes(p) for p in pages]
    buckets = {}
    for i, (sprite, group) in enumerate(zip(sprites, groups)):
        p, x, y, w, h = sprite[:5]
        if (p < 0 or p >= len(pages) or min(w, h) <= 0 or x < room.PAD or y < room.PAD or
                x + w + room.PAD > pages[p].width or y + h + room.PAD > pages[p].height):
            raise ValueError(f"Invalid padded sprite {i}")
        buckets.setdefault(group, {}).setdefault(palettes[p], []).append(i)
    output, records, metadata = [], list(sprites), []
    for group, by_palette in buckets.items():
        for palette, ids in by_palette.items():
            if canonical_views:
                places, count = pack_canonical(pages, sprites, ids, page_size)
            else:
                places, count = room.pack([(sprites[i][3], sprites[i][4]) for i in ids], page=page_size)
            first = len(output)
            for _ in range(count):
                image = Image.new("P", (page_size, page_size))
                image.putpalette(palette, rawmode="RGBA")
                output.append(image)
                metadata.append(dict(group=group, source_pages=[]))
            for i, (p, x, y) in zip(ids, places):
                old, ox, oy, w, h = sprites[i][:5]
                # Copy the existing border too. Rebuilding it from RGBA would
                # permit palette conversion and lose the exact index contract.
                border = room.PAD
                crop = pages[old].crop((ox-border, oy-border, ox+w+border, oy+h+border))
                output[first+p].paste(crop, (x-border, y-border))
                records[i] = (first+p, x, y, *sprites[i][3:])
                if old not in metadata[first+p]["source_pages"]:
                    metadata[first+p]["source_pages"].append(old)
    verify(pages, sprites, output, records, canonical_views=canonical_views)
    return output, records, metadata


def verify(before, old_sprites, after, new_sprites, *, canonical_views=False):
    """Fail closed on a missing frame, geometry change, CLUT or filter-border drift."""
    if len(old_sprites) != len(new_sprites):
        raise ValueError("Sprite count changed during repacking")
    old_palettes = [palette_bytes(p) for p in before]
    new_palettes = [palette_bytes(p) for p in after]
    for i, (old, new) in enumerate(zip(old_sprites, new_sprites)):
        if old[3:] != new[3:] or old_palettes[old[0]] != new_palettes[new[0]]:
            raise ValueError(f"Sprite {i} geometry or CLUT changed")
        crops = []
        for pages, s in ((before, old), (after, new)):
            p, x, y, w, h = s[:5]
            b = room.PAD
            crops.append(pages[p].crop((x-b, y-b, x+w+b, y+h+b)).tobytes())
        if crops[0] != crops[1]:
            raise ValueError(f"Sprite {i} padded indices changed")
        if canonical_views:
            source = windows.safe_window(*before[old[0]].size, 8, old[1:5], room.PAD)
            if windows.relocated_window(source, *after[new[0]].size, 8,
                                        new[1:5], room.PAD) is None:
                raise ValueError(f"Sprite {i} canonical texture view changed")


def read_pages(Image, directory, count):
    pages = []
    for p in range(count):
        raw = (directory / f"page-{p}.hpt").read_bytes()
        if len(raw) < 1040 or raw[:4] != b"HPT1":
            raise ValueError("Invalid baseline HPT1")
        w, h, bits, clut, swizzled, reserved, entries = struct.unpack_from("<HHBBBBI", raw, 4)
        if ((bits, clut, swizzled, reserved, entries) != (5, 3, 1, 0, 256) or
                not 16 <= w <= 512 or not 8 <= h <= 512 or
                w & (w-1) or h & (h-1) or len(raw) != 1040 + w*h):
            raise ValueError("Expected exact swizzled CLUT8 HPT1")
        image = Image.frombytes("P", (w, h), room.unswizzle(raw[1040:], w, h))
        image.putpalette(raw[16:1040], rawmode="RGBA")
        pages.append(image)
    return pages


def read_bank(raw, *, with_cluts=False):
    """Validate the runtime PSR1/PSA1 contract before copying any opaque tail."""
    if len(raw) < 28 or raw[:4] not in (b"PSA1", b"PSR1"):
        raise ValueError("Expected converted PSA1/PSR1 table")
    version, size, count, sprites, a, b = struct.unpack_from("<6I", raw, 4)
    if with_cluts:
        footer = len(raw)-8-4*sprites
        if (footer < 28 or raw[footer:footer+4] != b"SCL1" or
                struct.unpack_from("<I", raw, footer+4)[0] != sprites or
                any(i >= 128 for i, in struct.iter_unpack("<I", raw[footer+8:]))):
            raise ValueError("Invalid per-sprite CLUT footer")
        raw = raw[:footer]
    end = 28 + sprites*36
    if (version != 1 or size != room.PAGE or not 1 <= count <= 128 or
            not 1 <= sprites <= 65536 or not 1 <= a <= 65536 or b > 1048576 or
            end > len(raw)):
        raise ValueError("Invalid converted bank header")
    records = [struct.unpack_from("<5I4f", raw, 28+i*36) for i in range(sprites)]
    for s in records:
        p, x, y, w, h, left, bottom, right, top = s
        if (p >= count or not w or not h or x+w > size or y+h > size or
                not all(math.isfinite(v) and abs(v) <= 100000 for v in s[5:]) or
                left >= right or bottom >= top):
            raise ValueError("Invalid sprite geometry")
    if raw[:4] == b"PSR1":
        if b > a or len(raw) != end+72*a+24*b+4:
            raise ValueError("Invalid room tail size")
        for i in range(a):
            sprite, owner, *v = struct.unpack_from("<2I16f", raw, end+72*i)
            if (sprite >= sprites or not all(math.isfinite(f) and abs(f) <= 100000 for f in v) or
                    not 0 <= v[15] <= 7 or v[15] != int(v[15]) or
                    any(not 0 <= f <= 1 for f in v[7:11])):
                raise ValueError("Invalid room draw")
        seen = set()
        for i in range(b):
            index, *v = struct.unpack_from("<I5f", raw, end+72*a+24*i)
            if index >= a or index in seen or not all(math.isfinite(f) for f in v):
                raise ValueError("Invalid room grass")
            seen.add(index)
        depth, = struct.unpack_from("<f", raw, len(raw)-4)
        minimum, maximum = struct.unpack("<2f", struct.pack("<2f", -28.1, 961.9))
        if not math.isfinite(depth) or not minimum <= depth <= maximum:
            raise ValueError("Invalid room split depth")
    else:
        tail_end = end+68*a+4*b
        if not b or tail_end > len(raw):
            raise ValueError("Invalid animation tail size")
        for i in range(a):
            name, fps, wrap, loop, first, length = struct.unpack_from("<48sf4I", raw, end+68*i)
            if (b"\0" not in name or not math.isfinite(fps) or fps <= 0 or
                    wrap not in (0, 1, 2, 3, 6) or not length or first+length > b or
                    (wrap == 1 and loop >= length)):
                raise ValueError("Invalid animation clip")
        if any(f >= sprites for f, in struct.iter_unpack("<I", raw[end+68*a:tail_end])):
            raise ValueError("Invalid animation frame")
        if tail_end != len(raw):
            if len(raw)-tail_end < 8 or raw[tail_end:tail_end+4] != b"AEV1":
                raise ValueError("Invalid animation events")
            events, = struct.unpack_from("<I", raw, tail_end+4)
            if events > b or len(raw) != tail_end+8+76*events:
                raise ValueError("Invalid animation events size")
            previous = -1
            for i in range(events):
                frame, info, integer, value = struct.unpack_from("<I64sIf", raw, tail_end+8+76*i)
                if frame <= previous or frame >= b or b"\0" not in info or not math.isfinite(value):
                    raise ValueError("Invalid animation event")
                previous = frame
    return count, records, end


def read_layout_bank(raw, *, with_cluts=False):
    """Expose existing PSR1 v2 tiles without changing their logical geometry.

    The route loader derives tile geometry from logical texel partitions. Keep
    that mapping and the original table hash during planning and export.
    Camera admission uses the full logical sprite rather than individual tiles.
    """
    if len(raw) < 28 or raw[:4] != b'PSR1' or struct.unpack_from('<I', raw, 4)[0] != 2:
        count, sprites, end = read_bank(raw, with_cluts=with_cluts)
        return dict(count=count, sprites=sprites, end=end, logical_sprites=sprites,
                    logical_parts=[[i] for i in range(len(sprites))],
                    logical_regions=[[(0, 0)] for _ in sprites], source_version=1,
                    base_end=len(raw)-(8+4*len(sprites) if with_cluts else 0),
                    cluts=list(struct.unpack(f"<{len(sprites)}I", raw[-4*len(sprites):])) if with_cluts else None)
    _, size, count, n, draws, grass = struct.unpack_from('<6I', raw, 4)
    end = 28+36*n
    base_end = end+72*draws+24*grass+4
    if not (size == 512 and 1 <= count <= 128 and 1 <= n <= 65536 and
            1 <= draws <= 65536 and grass <= draws and base_end+4 <= len(raw)):
        raise ValueError("Invalid tiled room header/tail")
    logical = [struct.unpack_from('<5I4f', raw, 28+36*i) for i in range(n)]
    # Validate all draw/grass/depth and logical geometry fields with the v1
    # reader; only logical UV dimensions may exceed a page in the v2 contract.
    sanitized = b''.join(struct.pack('<5I4f', s[0], 1, 1, 1, 1, *s[5:]) for s in logical)
    header = b'PSR1'+struct.pack('<6I', 1, size, count, n, draws, grass)
    read_bank(header+sanitized+raw[end:base_end])
    if any(not (s[0] < count and 0 < s[3] <= 16384 and 0 < s[4] <= 16384) for s in logical):
        raise ValueError("Invalid logical sprite")
    offset = base_end
    def take(fmt):
        nonlocal offset
        length = struct.calcsize(fmt)
        if offset+length > len(raw):
            raise ValueError("Truncated tiled room")
        values = struct.unpack_from(fmt, raw, offset)
        offset += length
        return values
    tiled_count, = take('<I')
    if not 1 <= tiled_count <= n:
        raise ValueError("Invalid tiled sprite count")
    tiled, tiled_regions, previous = {}, {}, -1
    for _ in range(tiled_count):
        index, parts = take('<2I')
        if not previous < index < n or not 2 <= parts <= 4096:
            raise ValueError("Invalid tile index/count")
        previous = index
        full = logical[index]
        regions, records = [], []
        for _ in range(parts):
            x, y, *s = take('<7I4f')
            page, u, v, w, h = s[:5]
            if (page >= count or not w or not h or u+w > size or v+h > size or
                    x+w > full[3] or y+h > full[4] or
                    not all(math.isfinite(f) and abs(f) <= 100000 for f in s[5:]) or
                    s[5] >= s[7] or s[6] >= s[8]):
                raise ValueError("Invalid tile rectangle/geometry")
            if any(x < a+c and a < x+w and y < b+d and b < y+h for a, b, c, d in regions):
                raise ValueError("Overlapping logical tiles")
            regions.append((x, y, w, h))
            records.append(tuple(s))
        if sum(w*h for _, _, w, h in regions) != full[3]*full[4]:
            raise ValueError("Incomplete logical tile coverage")
        tiled[index] = records
        tiled_regions[index] = [(x, y) for x, y, _, _ in regions]
    sprites, mapping = [], []
    for i, s in enumerate(logical):
        records = tiled.get(i, [s])
        if i not in tiled and (s[1]+s[3] > size or s[2]+s[4] > size):
            raise ValueError("Untiled logical sprite exceeds page")
        mapping.append(list(range(len(sprites), len(sprites)+len(records))))
        sprites.extend(records)
    cluts = None
    if with_cluts:
        expected = 8+4*len(sprites)
        if (len(raw)-offset != expected or raw[offset:offset+4] != b'SCL1' or
                struct.unpack_from('<I', raw, offset+4)[0] != len(sprites)):
            raise ValueError("Invalid tiled per-sprite CLUT footer")
        cluts = list(struct.unpack_from(f'<{len(sprites)}I', raw, offset+8))
        if any(c >= 128 for c in cluts):
            raise ValueError("Invalid tiled per-sprite CLUT index")
    elif offset != len(raw):
        raise ValueError("Extra tiled room bytes")
    return dict(count=count, sprites=sprites, end=end, base_end=base_end, cluts=cluts, logical_sprites=logical,
                logical_parts=mapping,
                logical_regions=[tiled_regions.get(i, [(0, 0)]) for i in range(n)], source_version=2)


def convert(table, profile, output):
    """Repack a converted bank; copy its clips/frames/events/draw tail verbatim."""
    table, output = Path(table), Path(output)
    if output.exists():
        raise ValueError("Output must be a new directory; retain the baseline")
    raw = table.read_bytes()
    count, records, end = read_bank(raw)
    sprites = len(records)
    digest = hashlib.sha256(raw).hexdigest()
    if profile.get("table_sha256") != digest:
        raise ValueError("Packing profile does not match baseline table SHA256")
    Image = room.import_pil()
    pages = read_pages(Image, table.parent, count)
    canonical_views = profile.get("canonical_views", False)
    if type(canonical_views) is not bool:
        raise ValueError("canonical_views must be boolean")
    packed, remapped, metadata = repack(Image, pages, records, profile.get("groups", []),
                                        canonical_views=canonical_views)
    if len(packed) > 128:
        raise ValueError("Repacked bank exceeds the runtime's 128-page limit")
    data = bytearray(raw[:28])
    struct.pack_into("<I", data, 12, len(packed))
    for s in remapped:
        data += struct.pack("<5I4f", *s)
    data += raw[end:]
    report = dict(table=table.name, table_sha256=digest, sprites=sprites,
                  before_pages=count, after_pages=len(packed), pages=metadata,
                  texture_bytes=len(packed)*(room.PAGE*room.PAGE+1040),
                  verification="all sprite CLUTs, geometry and padded indices byte-identical")
    report["canonical_views"] = canonical_views
    if canonical_views:
        # Offline evidence only: no new runtime metadata or allocation.
        report["source_views"] = [windows.safe_window(*pages[s[0]].size, 8, s[1:5], room.PAD)
                                  for s in records]
        report["destination_views"] = [
            windows.relocated_window(v, *packed[s[0]].size, 8, s[1:5], room.PAD)
            for v, s in zip(report["source_views"], remapped)]
    # IDs are unchanged, so bank sidecars remain byte-identical. The baseline
    # converter manifest describes its old atlas; repack.json replaces it.
    sidecars = [p for p in sorted(table.parent.iterdir()) if p.is_file() and
                p != table and p.name not in ("manifest.json", "repack.json") and
                not re.fullmatch(r"page-\d+\.(hpt|png)", p.name)]
    report["sidecars_sha256"] = {p.name: room.sha256(p) for p in sidecars}
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="repack-", dir=output.parent) as temporary:
        staging = Path(temporary)/"bank"
        staging.mkdir()
        for p, image in enumerate(packed):
            (staging/f"page-{p}.hpt").write_bytes(
                room.hpt1(*image.size, palette_bytes(image), image.tobytes()))
            image.convert("RGBA").save(staging/f"page-{p}.png")
        (staging/table.name).write_bytes(data)
        for sidecar in sidecars:
            shutil.copy2(sidecar, staging/sidecar.name)
        (staging/"repack.json").write_text(json.dumps(report, indent=2)+"\n", encoding="utf-8")
        # Re-read the serialized HPTs too; the in-memory guard alone does not
        # exercise swizzling, file headers or output palette serialization.
        read_bank((staging/table.name).read_bytes())
        verify(pages, records, read_pages(Image, staging, len(packed)), remapped,
               canonical_views=canonical_views)
        staging.rename(output)
    return report


def restore_mixed_layout(pages, records, profile):
    """Serialize the measured camera layout without running a different packer."""
    layout = profile['layout']
    if layout['profile'] != {k: v for k, v in profile.items() if k != 'layout'}:
        raise ValueError("Measured layout profile mismatch")
    width, height = pages[0].size
    count = layout['after_pages']
    sprites = [tuple(s) for s in layout['sprite_records']]
    if not 1 <= count <= 128 or len(sprites) != len(records):
        raise ValueError("Measured layout page/sprite count invalid")
    # Integer atlas translations can cross the GE sampler's fractional boundary.
    # Keep measured sensitive sprites at their source UVs even on another page.
    original_uv = layout.get('original_uv_sprites', [])
    if (not isinstance(original_uv, list) or
            any(type(i) is not int or not 0 <= i < len(records) for i in original_uv) or
            len(set(original_uv)) != len(original_uv)):
        raise ValueError("Measured layout original UV sprite IDs invalid")
    if any(sprites[i][1:3] != records[i][1:3] for i in original_uv):
        raise ValueError("Measured layout original UV changed")
    indices = [bytearray(width*height) for _ in range(count)]
    for old, new in zip(records, sprites):
        p, x, y, w, h = new[:5]
        if (old[3:] != new[3:] or not 0 <= p < count or
                min(x, y) < room.PAD or x+w+room.PAD > width or y+h+room.PAD > height):
            raise ValueError("Measured layout geometry invalid")
        source, ox, oy = old[:3]
        pad = room.PAD
        if (min(ox, oy) < pad or ox+w+pad > pages[source].width or oy+h+pad > pages[source].height):
            raise ValueError("Measured layout source padding invalid")
        crop = pages[source].crop((ox-pad, oy-pad, ox+w+pad, oy+h+pad)).tobytes()
        stride = w+2*pad
        for row in range(h+2*pad):
            start = (y-pad+row)*width+x-pad
            indices[p][start:start+stride] = crop[row*stride:(row+1)*stride]
    plan = dict(size=(width, height), indices=[bytes(p) for p in indices],
        palettes=list(dict.fromkeys(palette_bytes(p) for p in pages)), sprites=sprites,
        cluts=layout['sprite_cluts'], groups=layout['page_groups'],
        algorithm=profile['algorithm'])
    verify_mixed(pages, records, profile['groups'], plan)
    for key, payload in [('index_sha256', plan['indices']), ('clut_sha256', plan['palettes'])]:
        if [hashlib.sha256(p).hexdigest() for p in payload] != layout[key]:
            raise ValueError("Measured layout payload SHA256 mismatch")
    return plan


def serialize_mixed_table(raw, bank, plan):
    """Keep logical IDs and all draw/grass records; relocate physical tiles only.

    In v2 SCL1 is in flattened logical/part order, exactly as the planner's
    sprite_records. Logical dimensions/geometry and tile regions stay intact.
    """
    data = bytearray(raw[:28])
    struct.pack_into('<I', data, 12, len(plan['indices']))
    if bank['source_version'] == 1:
        for sprite in plan['sprites']:
            data += struct.pack('<5I4f', *sprite)
        data += raw[bank['end']:]
    else:
        for logical, parts in zip(bank['logical_sprites'], bank['logical_parts']):
            first = plan['sprites'][parts[0]]
            data += struct.pack('<5I4f', *first[:3], *logical[3:])
        data += raw[bank['end']:bank['base_end']]
        tiled = [i for i, parts in enumerate(bank['logical_parts']) if len(parts) > 1]
        data += struct.pack('<I', len(tiled))
        for i in tiled:
            parts = bank['logical_parts'][i]
            data += struct.pack('<2I', i, len(parts))
            for region, part in zip(bank['logical_regions'][i], parts):
                data += struct.pack('<7I4f', *region, *plan['sprites'][part])
    data += struct.pack('<4sI', b'SCL1', len(plan['sprites']))
    data += struct.pack(f"<{len(plan['sprites'])}I", *plan['cluts'])
    return bytes(data)


def convert_mixed(table, profile, output):
    """Export a v1 bank or v2 logical/tile candidate without flattening draws.

    HPT2 holds only swizzled T8 indices. PCL1 stores the original RGBA CLUTs;
    SCL1 follows the unchanged room or animation tail (including AEV1).
    A profile with a 'layout' bank from route-mixed-layout.json retains that
    measured relocation and checks its index/CLUT hashes. No shipping policy
    or cross-owner page namespace is inferred here.
    """
    table, output = Path(table), Path(output)
    if output.exists():
        raise ValueError("Output must be a new directory; retain the baseline")
    raw = table.read_bytes()
    bank = read_layout_bank(raw)
    count, records = bank['count'], bank['sprites']
    digest = hashlib.sha256(raw).hexdigest()
    if profile.get("table_sha256") != digest:
        raise ValueError("Packing profile does not match baseline table SHA256")
    if profile.get("canonical_views", False):
        raise ValueError("Mixed export uses relocated full-page views")
    if 'page_sha256' in profile or 'layout' in profile:
        if profile.get('page_sha256') != [room.sha256(table.parent/f'page-{i}.hpt') for i in range(count)]:
            raise ValueError("Packing profile source page SHA256 mismatch")
    Image = room.import_pil()
    pages = read_pages(Image, table.parent, count)
    groups = profile.get("groups", [])
    if bank['source_version'] == 2 and 'layout' in profile:
        for key in ('source_version', 'logical_sprites', 'logical_parts', 'logical_regions'):
            if json.dumps(profile['layout'].get(key)) != json.dumps(bank[key]):
                raise ValueError(f"Measured layout logical mapping mismatch: {key}")
    plan = (restore_mixed_layout(pages, records, profile) if 'layout' in profile else
            pack_mixed(pages, records, groups))
    if len(plan['indices']) > 128 or not 1 <= len(plan['palettes']) <= 128:
        raise ValueError("Mixed bank exceeds the runtime's page/CLUT limit")
    data = serialize_mixed_table(raw, bank, plan)
    sidecars = [p for p in sorted(table.parent.iterdir()) if p.is_file() and
                p != table and p.name not in ("manifest.json", "repack.json", "palettes.pcl") and
                not re.fullmatch(r"page-\d+\.(hpt|png)", p.name)]
    report = dict(table=table.name, table_sha256=digest, sprites=len(records),
                  source_version=bank['source_version'], logical_sprites=len(bank['logical_sprites']),
                  tiled_sprites=sum(len(parts) > 1 for parts in bank['logical_parts']),
                  before_pages=count, after_pages=len(plan['indices']),
                  palettes=len(plan['palettes']), pages=plan['groups'],
                  format="HPT2/PCL1/SCL1", algorithm=plan['algorithm'],
                  texture_bytes=sum(16+len(p) for p in plan['indices'])+
                                12+1024*len(plan['palettes']),
                  sidecars_sha256={p.name: room.sha256(p) for p in sidecars},
                  verification="all sprite CLUTs, geometry and padded indices byte-identical")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="repack-", dir=output.parent) as temporary:
        staging = Path(temporary)/"bank"
        staging.mkdir()
        w, h = plan['size']
        for i, indices in enumerate(plan['indices']):
            header = struct.pack("<4sHH4BI", b"HPT2", w, h, 5, 3, 1, 0, len(plan['palettes']))
            (staging/f"page-{i}.hpt").write_bytes(header+room.swizzle(indices, w, h))
        (staging/"palettes.pcl").write_bytes(
            struct.pack("<4sII", b"PCL1", 1, len(plan['palettes']))+b''.join(plan['palettes']))
        (staging/table.name).write_bytes(data)
        for sidecar in sidecars:
            shutil.copy2(sidecar, staging/sidecar.name)
        (staging/"repack.json").write_text(json.dumps(report, indent=2)+"\n", encoding="utf-8")
        # Audit serialized bytes, not just the in-memory plan.
        serialized_table = (staging/table.name).read_bytes()
        exported = read_layout_bank(serialized_table, with_cluts=True)
        if (exported['count'] != len(plan['indices']) or
                exported['logical_parts'] != bank['logical_parts'] or
                exported['logical_regions'] != bank['logical_regions'] or
                [s[3:] for s in exported['logical_sprites']] != [s[3:] for s in bank['logical_sprites']] or
                serialized_table[exported['end']:exported['base_end']] != raw[bank['end']:bank['base_end']]):
            raise ValueError('Serialized logical mapping, geometry or room tail changed')
        actual = dict(plan, sprites=exported['sprites'], cluts=exported['cluts'],
                      palettes=[(staging/"palettes.pcl").read_bytes()[12+i*1024:12+(i+1)*1024]
                                for i in range(len(plan['palettes']))],
                      indices=[room.unswizzle((staging/f"page-{i}.hpt").read_bytes()[16:], w, h)
                               for i in range(len(plan['indices']))])
        verify_mixed(pages, records, groups, actual)
        staging.rename(output)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("table", type=Path)
    parser.add_argument("profile", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--multi-clut", action="store_true", help="Export an HPT2/PCL1/SCL1 candidate")
    args = parser.parse_args()
    if args.output.resolve() == args.table.parent.resolve():
        parser.error("Use a separate output directory to retain the baseline")
    converter = convert_mixed if args.multi_clut else convert
    result = converter(args.table, json.loads(args.profile.read_text()), args.output)
    print(f"{result['sprites']} sprites verified; {result['before_pages']} -> {result['after_pages']} pages")


if __name__ == "__main__":
    main()
