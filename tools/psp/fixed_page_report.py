"""Offline fixed-UV multi-CLUT packing and conservative static room windows.

No shipping data is written. A trace's unused sprites stay separately loadable.
Pinned sets are observed shared-library unions, not proof of full-route use.
"""
import csv
import hashlib
import math
from pathlib import Path
import struct

import repack_pages as pack
import texture_window_report as windows


def rectangle(sprite):
    _, x, y, w, h = sprite[:5]
    pad = pack.room.PAD
    return x-pad, y-pad, x+w+pad, y+h+pad


def conflicts(pages, sprites, ids):
    """An edge proves two fixed-coordinate sprites cannot share index storage."""
    width = pages[0].width
    source = [p.tobytes() for p in pages]
    graph = {i: set() for i in ids}
    for offset, i in enumerate(ids):
        a, b = sprites[i][0], rectangle(sprites[i])
        for j in ids[offset+1:]:
            c, d = sprites[j][0], rectangle(sprites[j])
            if a == c:
                continue
            left, top = max(b[0], d[0]), max(b[1], d[1])
            right, bottom = min(b[2], d[2]), min(b[3], d[3])
            if left >= right or top >= bottom:
                continue
            if any(source[a][r*width+left:r*width+right] !=
                   source[c][r*width+left:r*width+right] for r in range(top, bottom)):
                graph[i].add(j)
                graph[j].add(i)
    return graph


def clique(graph, ids):
    """A deterministic witnessed lower bound, never an optimal-coloring claim."""
    ids = set(ids)
    best = []
    for first in sorted(ids, key=lambda i: (-len(graph[i] & ids), i))[:16]:
        chosen, candidates = [first], graph[first] & ids
        while candidates:
            i = min(candidates, key=lambda i: (-len(graph[i] & candidates), i))
            chosen.append(i)
            candidates &= graph[i]
        if len(chosen) > len(best):
            best = chosen
    if any(j not in graph[i] for n, i in enumerate(best) for j in best[n+1:]):
        raise AssertionError("Invalid fixed-coordinate clique witness")
    return best


def room_draws(raw, end):
    if raw[:4] != b'PSR1':
        raise ValueError("Room table must be PSR1")
    count, grass_count = struct.unpack_from('<2I', raw, 20)
    draws = [struct.unpack_from('<2I16f', raw, end+72*i) for i in range(count)]
    grass = {}
    for i in range(grass_count):
        index, *values = struct.unpack_from('<I5f', raw, end+72*count+24*i)
        grass[index] = values
    return draws, grass


def camera_bounds(sprite, values, grass, margin):
    """Mirror roomCameraBounds at 480x272, with a pixel margin at native focal.

    Conservative corner/grass bounds; opacity and far-depth rejects are omitted.
    Static table transforms only: runtime scale/switch/visibility changes need
    their own route evidence before this can be a loader acceptance report.
    """
    if not math.isfinite(margin) or margin < 0:
        raise ValueError("Window margin must be finite and nonnegative")
    fov = 24*max(1, (16/9)/max(1.6, 480/272))
    focal = 272/(2*math.tan(fov*math.pi/360))
    left, bottom, right, top = math.inf, math.inf, -math.inf, -math.inf
    _, amplitude, _, push, height = grass
    for k in (push-abs(amplitude), push+abs(amplitude)):
        v = list(values)
        for a, b, c in ((0, 1, 2), (3, 4, 5), (13, 14, 6)):
            v[b] += k*v[a]
            v[c] += k*height*v[a]
        for x in (sprite[5], sprite[7]):
            for y in (sprite[6], sprite[8]):
                depth = max(10, 38.1+v[6]+v[13]*x+v[14]*y)
                wx, wy = v[0]*x+v[1]*y+v[2], v[3]*x+v[4]*y+v[5]
                dx, dy = (241+margin)*depth/focal, (137+margin)*depth/focal
                left, bottom = min(left, wx-dx), min(bottom, wy-dy)
                right, top = max(right, wx+dx), max(top, wy+dy)
    return (math.nextafter(left, -math.inf), math.nextafter(bottom, -math.inf),
            math.nextafter(right, math.inf), math.nextafter(top, math.inf))


def usage(trace, tables):
    """Match exact UVs to baseline sprite IDs and cross-check every page row."""
    assets = dict(line.split('\t', 1) for line in (trace/'assets.tsv').read_text().splitlines())
    banks = trace/'page-banks.csv'
    if banks.exists():
        # Current hosts always write the bank table; one bank of one scene is a single-room trace.
        with banks.open(newline='', encoding='utf-8-sig') as stream:
            rows = list(csv.DictReader(stream))
        if len({(r['bank'], r['scene']) for r in rows}) != 1:
            raise ValueError("Fixed-layout report currently requires a single-room trace")
    paths = [Path(path).resolve() for name, path in assets.items() if name.endswith('.hpt')]
    room = Path(assets['static-room-visual.bin']).resolve()
    lookup = {}
    for name, bank in tables.items():
        for i, sprite in enumerate(bank['sprites']):
            p, x, y, w, h = sprite[:5]
            key = (bank['table'].parent/f'page-{p}.hpt').resolve(), x, y, x+w, y+h
            lookup.setdefault(key, set()).add((name, i))
    frames, original = [], []
    with (trace/'page-trace.csv').open(newline='', encoding='utf-8-sig') as stream:
        for i, row in enumerate(csv.DictReader(stream)):
            if int(row['frame']) != i or int(row.get('bank', 0)) != 0:
                raise ValueError("Page trace must have contiguous single-bank frames")
            original.append({int(p) for p in row['pages'].split(';') if p})
            frames.append(set())
    if not frames:
        raise ValueError("Empty page trace")
    seen, untouched = [set() for _ in frames], set()
    previous = -1
    with (trace/'sprite-trace.csv').open(newline='', encoding='utf-8-sig') as stream:
        for row in csv.DictReader(stream):
            frame, page = int(row['frame']), int(row['page'])
            if not previous <= frame < len(frames) or frame < 0 or not 0 <= page < len(paths):
                raise ValueError("Invalid sprite frame/page ordering")
            if int(row.get('bank', 0)) != 0:
                raise ValueError("Unknown sprite bank")
            previous = frame
            uv = [float(row[k]) for k in ('u0', 'v0', 'u1', 'v1')]
            if (uv[0] == uv[2] and uv[1] == uv[3] and all(map(math.isfinite, uv)) and
                    not any(paths[page].parent == b['table'].parent for b in tables.values())):
                # A texel-centre point sample (the font page's solid fill for boxes) of
                # an owner this plan does not repack: that owner keeps its pages.
                untouched.add(paths[page])
                frames[frame].add(('untouched', str(paths[page])))
                seen[frame].add(page)
                previous = frame
                continue
            if any(not math.isfinite(v) or v != int(v) for v in uv):
                raise ValueError("Expected integral finite UV edges")
            u, v, s, t = map(int, uv)
            key = paths[page], min(u, s), min(v, t), max(u, s), max(v, t)
            if key[1] >= key[3] or key[2] >= key[4]:
                raise ValueError("Empty sprite rectangle")
            hits = lookup.get(key)
            if hits is None:
                # Entire owners without a supported table (fonts and light)
                # retain their original pages. A missing sprite in a known
                # bank must fail instead of silently escaping the packing.
                if any(key[0].parent == b['table'].parent for b in tables.values()):
                    raise ValueError(f"Unmatched sprite in converted bank: {key}")
                untouched.add(paths[page])
                frames[frame].add(('untouched', str(paths[page])))
            else:
                frames[frame].update(hits)
            seen[frame].add(page)
    if seen != original:
        raise ValueError("Sprite/page trace mismatch")
    return frames, original, paths, room, untouched


def camera_windows(trace, frames, room_name, bank, margin):
    """Every recorded camera footprint, unioned with actually submitted sprites."""
    with (Path(trace)/'metrics.csv').open(newline='', encoding='utf-8-sig') as stream:
        cameras = list(csv.DictReader(stream))
    if len(cameras) != len(frames) or any(int(r['frame']) != i for i, r in enumerate(cameras)):
        raise ValueError("Camera trace does not match page frames")
    return camera_footprints(frames, cameras, room_name, bank, margin)


def camera_footprints(frames, cameras, room_name, bank, margin):
    """Evaluate selected visits after their producer's global frame validation."""
    if len(cameras) != len(frames):
        raise ValueError("Camera trace does not match page frames")
    draws, grass = room_draws(bank['raw'], bank['end'])
    bounds = [(sprite, camera_bounds(bank['sprites'][sprite], v,
                                    grass.get(i, [0]*5), margin))
              for i, (sprite, _, *v) in enumerate(draws)]
    footprints = []
    for ids, camera in zip(frames, cameras):
        x, y = float(camera['camera_x']), float(camera['camera_y'])
        if not math.isfinite(x) or not math.isfinite(y):
            raise ValueError("Nonfinite camera position")
        active = {i for name, i in ids if name == room_name}
        active.update(i for i, b in bounds if b[0] <= x <= b[2] and b[1] <= y <= b[3])
        footprints.append(frozenset(active))
    return footprints


def analyze(trace, manifest, cell_size=0, margin=64):
    """Measure grouped packing upper bounds and fixed-coordinate clique bounds."""
    if not math.isfinite(cell_size) or cell_size < 0:
        raise ValueError("Cell size must be finite and nonnegative")
    if not math.isfinite(margin) or margin < 0:
        raise ValueError("Window margin must be finite and nonnegative")
    trace = Path(trace)
    Image = pack.room.import_pil()
    tables = {}
    for name, table in windows.banks(Path(manifest)).items():
        raw = table.read_bytes()
        count, sprites, end = pack.read_bank(raw)
        tables[name] = dict(table=table.resolve(), raw=raw, count=count, sprites=sprites, end=end)
    frames, original, paths, room, untouched = usage(trace, tables)
    room_name = next((name for name, b in tables.items() if b['table'] == room), None)
    if room_name is None:
        raise ValueError("Static room table missing from bank manifest")
    footprints = camera_windows(trace, frames, room_name, tables[room_name], margin)
    used = set().union(*frames)
    remap, bank_reports, graphs = {}, [], {}
    pinned, pinned_lower = set(), len(untouched)
    for name, bank in tables.items():
        sprites, raw = bank['sprites'], bank['raw']
        active = [i for i in range(len(sprites)) if (name, i) in used]
        spatial, positions = {}, {}
        is_room = bank['table'] == room
        if is_room:
            room_name = name
            draws, grass = room_draws(raw, bank['end'])
            for sprite, _, *v in draws:
                positions.setdefault(sprite, []).append((v[2], v[5]))
            if cell_size:
                for i, xy in positions.items():
                    spatial[i] = (math.floor(sum(x for x, y in xy)/len(xy)/cell_size),
                                  math.floor(sum(y for x, y in xy)/len(xy)/cell_size))
        groups = [('used' if (name, i) in used else 'unused') +
                  (f'/cell/{spatial[i][0]}/{spatial[i][1]}' if i in spatial else '')
                  for i in range(len(sprites))]
        pages = pack.read_pages(Image, bank['table'].parent, bank['count'])
        plan = pack.pack_fixed(pages, sprites, groups)
        graph = conflicts(pages, sprites, active)
        graphs[name] = graph
        witness = clique(graph, active)
        for i, sprite in enumerate(plan['sprites']):
            remap[name, i] = (name, sprite[0])
            if not is_room and i in active:
                pinned.add((name, sprite[0]))
        if not is_room:
            pinned_lower += len(witness)
        bank_reports.append(dict(bank=name, room=is_room, sprites=len(sprites), used_sprites=len(active),
            before_pages=bank['count'], after_pages=len(plan['indices']),
            used_pages=len({plan['sprites'][i][0] for i in active}),
            fixed_coordinate_clique=witness, table_sha256=hashlib.sha256(raw).hexdigest(),
            profile=dict(groups=groups, table_sha256=hashlib.sha256(raw).hexdigest()),
            sprite_pages=[s[0] for s in plan['sprites']], sprite_cluts=plan['cluts'],
            index_sha256=[hashlib.sha256(p).hexdigest() for p in plan['indices']],
            clut_sha256=[hashlib.sha256(p).hexdigest() for p in plan['palettes']]))
    pinned.update(('untouched', str(p)) for p in untouched)
    window_max, old_window_max, room_max, room_lower, witness_frame, room_witness = 0, 0, 0, 0, None, []
    room_bank = tables[room_name]
    for frame, (ids, footprint) in enumerate(zip(frames, footprints)):
        active = [i for name, i in ids if name == room_name]
        room_max = max(room_max, len({remap[room_name, i] for i in active}))
        # A witnessed lower bound needs no exhaustive coloring. Sample once
        # per second; all frames still contribute to packing/window maxima.
        proof = clique(graphs[room_name], active) if frame % 30 == 0 else []
        if len(proof) > room_lower:
            room_lower, witness_frame, room_witness = len(proof), frame, proof
        window = {remap[room_name, i] for i in footprint}
        window_max = max(window_max, len(window))
        old_window = {room_bank['sprites'][i][0] for i in footprint}
        old_window_max = max(old_window_max, len(old_window))
    room_paths = {p for p in paths if p.parent == room.parent}
    old_pinned = set().union(*original) - {i for i, p in enumerate(paths) if p in room_paths}
    old_room_max = max(len(ids-old_pinned) for ids in original)
    return dict(frames=len(frames), cell_size_world=cell_size, margin_pixels=margin,
        before=dict(observed_pinned_pages=len(old_pinned), max_visible_room_pages=old_room_max,
                    observed_pinned_plus_visible=len(old_pinned)+old_room_max,
                    max_static_grown_window_pages=old_window_max,
                    pinned_plus_static_window=len(old_pinned)+old_window_max),
        after=dict(observed_pinned_pages=len(pinned), max_visible_room_pages=room_max,
                   max_static_grown_window_pages=window_max, pinned_plus_static_window=len(pinned)+window_max),
        fixed_coordinate_lower_bound=dict(pinned=pinned_lower, visible_room=room_lower,
            total=pinned_lower+room_lower, witness_frame=witness_frame, room_clique=room_witness,
            witness_sample_stride=30, greedy_clique_starts=16),
        banks=bank_reports, unchanged_owners=sorted(str(p) for p in untouched),
        limitations=['Single-room tape; observed shared union is not full-route pinning',
                     'Static transform/grass windows plus observed draws; no unobserved runtime scale/switch states',
                     'Clique is a lower bound for fixed coordinates and separate owners only, not arbitrary relocation',
                     'No shipping format, rendering parity, runtime heap or hardware timing acceptance'])
