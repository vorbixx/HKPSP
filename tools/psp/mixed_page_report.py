"""Offline relocating multi-CLUT plans measured against complete camera tapes.

Observed shared unions are measured pinned sets, not full-route contracts.
Unused sprites remain on separate pages in their original owner banks.
"""
import csv
import hashlib
import json
import math
from pathlib import Path

import fixed_page_report as fixed
import repack_pages as pack
import texture_window_report as windows


def clut_transitions(trace, paths, palettes, frame_count):
    """Raw-palette transitions in draw order; blend variants are not traced.

    Reset the cache at each frame. Unsupported owners retain page identities.
    This is a traffic estimate for a future cache, not a runtime load counter.
    """
    counts, last = [0]*frame_count, {}
    with (Path(trace)/'sprite-trace.csv').open(newline='', encoding='utf-8-sig') as stream:
        for row in csv.DictReader(stream):
            frame, page = int(row['frame']), int(row['page'])
            path = paths[page]
            palette = palettes.get(path, ('unchanged', str(path)))
            if last.get(frame) != palette:
                counts[frame] += 1
                last[frame] = palette
    ordered = sorted(counts)
    return dict(raw_palette_changes_per_frame=counts, max=max(counts),
                p99=ordered[math.ceil(.99*len(ordered))-1],
                max_bytes_at_1024_per_clut=1024*max(counts),
                basis='Raw CLUT byte identity; reset per frame; unchanged owners by page identity',
                limitations='Blend variants, grade/mask passes, and runtime invalidations are not represented')


def analyze(trace, manifest, cell_size=0, margin=64):
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
    frames, original, paths, room, untouched = fixed.usage(trace, tables)
    room_name = next((n for n, b in tables.items() if b['table'] == room), None)
    if room_name is None:
        raise ValueError("Static room table missing from bank manifest")
    footprints = fixed.camera_windows(trace, frames, room_name, tables[room_name], margin)
    trace_hashes = {name: hashlib.sha256((trace/name).read_bytes()).hexdigest()
                    for name in ('assets.tsv', 'metrics.csv', 'page-trace.csv', 'sprite-trace.csv')}
    footprint_hash = hashlib.sha256(json.dumps([sorted(w) for w in footprints],
                                              separators=(',', ':')).encode()).hexdigest()
    used = set().union(*frames)
    remap, reports, source_palettes = {}, [], {}
    pinned = {('untouched', str(p)) for p in untouched}
    total_indices = total_palettes = 0
    for name, bank in tables.items():
        sprites = bank['sprites']
        active = {i for i in range(len(sprites)) if (name, i) in used}
        positions = {}
        if name == room_name and cell_size:
            draws, _ = fixed.room_draws(bank['raw'], bank['end'])
            for i, _, *v in draws:
                positions.setdefault(i, []).append((v[2], v[5]))
        groups = []
        for i in range(len(sprites)):
            group = 'used' if i in active else 'unused'
            if i in positions:
                points = positions[i]
                x = math.floor(sum(p[0] for p in points)/len(points)/cell_size)
                y = math.floor(sum(p[1] for p in points)/len(points)/cell_size)
                group += f'/cell/{x}/{y}'
            groups.append(group)
        pages = pack.read_pages(Image, bank['table'].parent, bank['count'])
        for i, page in enumerate(pages):
            path = (bank['table'].parent/f'page-{i}.hpt').resolve()
            source_palettes[path] = hashlib.sha256(pack.palette_bytes(page)).hexdigest()
        plan = pack.pack_mixed(pages, sprites, groups,
                               footprints if name == room_name else None)
        digest = hashlib.sha256(bank['raw']).hexdigest()
        for i, s in enumerate(plan['sprites']):
            remap[name, i] = (name, s[0])
            if name != room_name and i in active:
                pinned.add((name, s[0]))
        index_bytes = sum(map(len, plan['indices']))
        palette_bytes = sum(map(len, plan['palettes']))
        total_indices += index_bytes
        total_palettes += palette_bytes
        reports.append(dict(bank=name, room=name == room_name, sprites=len(sprites),
            used_sprites=len(active), before_pages=bank['count'], after_pages=len(plan['indices']),
            used_pages=len({plan['sprites'][i][0] for i in active}),
            moved_sprites=sum(a[:3] != b[:3] for a, b in zip(sprites, plan['sprites'])),
            profile=dict(table_sha256=digest, groups=groups,
                         page_sha256=[hashlib.sha256((bank['table'].parent/f'page-{i}.hpt').read_bytes()).hexdigest()
                                      for i in range(bank['count'])],
                         footprint_sha256=footprint_hash if name == room_name else None,
                         algorithm='first-window-area-maxrects' if name == room_name else 'shelf'),
            sprite_records=plan['sprites'], sprite_cluts=plan['cluts'], page_groups=plan['groups'],
            index_sha256=[hashlib.sha256(p).hexdigest() for p in plan['indices']],
            clut_sha256=[hashlib.sha256(p).hexdigest() for p in plan['palettes']],
            index_bytes=index_bytes, palette_bytes=palette_bytes))
    old_room = {i for i, p in enumerate(paths) if p.parent == room.parent}
    old_pinned = set().union(*original)-old_room
    room_sprites = tables[room_name]['sprites']
    old_window = max(len({room_sprites[i][0] for i in ids}) for ids in footprints)
    window_counts = [len({remap[room_name, i] for i in ids}) for ids in footprints]
    visible = max(len({remap[n, i] for n, i in ids if n == room_name}) for ids in frames)
    maximum = max(window_counts)
    return dict(frames=len(frames), cell_size_world=cell_size, margin_pixels=margin,
        trace_sha256=trace_hashes, window_pages_per_frame=window_counts,
        clut_traffic_estimate=clut_transitions(trace, paths, source_palettes, len(frames)),
        room=room_name,
        before=dict(observed_pinned_pages=len(old_pinned), max_static_grown_window_pages=old_window,
                    pinned_plus_static_window=len(old_pinned)+old_window),
        after=dict(observed_pinned_pages=len(pinned), max_visible_room_pages=visible,
                   max_static_grown_window_pages=maximum, pinned_plus_static_window=len(pinned)+maximum,
                   witness_frame=window_counts.index(maximum)),
        offline_payload=dict(indices_bytes=total_indices, palettes_bytes=total_palettes,
                             total_bytes=total_indices+total_palettes),
        banks=reports, unchanged_owners=sorted(str(p) for p in untouched),
        limitations=['Observed shared union is not full-route pinning',
            'Every static transform/grass footprint plus observed draws; unobserved dynamic states remain open',
            'Packing is an upper bound, not proof of the minimum number of slots',
            'Offline plan; shipping data, rendering parity, heap and hardware checks remain open'])
