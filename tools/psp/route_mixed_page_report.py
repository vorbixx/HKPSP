"""One relocating owner/use plan across every visit in a multi-room trace.

The JSON manifest explicitly maps staged table/page aliases to canonical banks.
Every alias is byte-validated; unknown textured owners fail closed. No shipping
assets are written. Slot counts are sufficient for this plan, not minima.

Manifest: banks contain name, table (relative to the manifest or absolute),
optional scene, and optional table_asset/page_assets aliases from assets.tsv.
Banks without aliases retain unobserved library versions. unchanged_assets
explicitly lists texture aliases retained at their original locations.
"""
import csv
import hashlib
import json
import math
from pathlib import Path
import struct

import fixed_page_report as fixed
import page_set_report as page_report
import repack_pages as pack


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def read_bank(raw):
    """Use the same logical/tile validation for planning and candidate export."""
    return pack.read_layout_bank(raw)


def load_banks(trace, manifest):
    """Admit canonical/staged aliases only after exact content comparison."""
    assets = {}
    for line in (trace/'assets.tsv').read_text().splitlines():
        name, path = line.split('\t', 1)
        path = Path(path).resolve()
        if name in assets and assets[name] != path:
            raise ValueError(f"Contradictory asset alias: {name}")
        assets[name] = path
    spec = json.loads(manifest.read_text())
    tables, aliases, rooms = {}, {}, {}
    for entry in spec['banks']:
        name = entry['name']
        if name in tables or name == 'untouched':
            raise ValueError(f"Duplicate/reserved bank: {name}")
        table = (manifest.parent/entry['table']).resolve()
        raw = table.read_bytes()
        bank = read_bank(raw)
        count = bank['count']
        scene = entry.get('scene')
        if scene is not None:
            if scene in rooms or raw[:4] != b'PSR1':
                raise ValueError("Duplicate scene or non-room scene bank")
            rooms[scene] = name
        tables[name] = dict(bank, table=table, raw=raw, scene=scene)
        page_assets = entry.get('page_assets', [])
        table_asset = entry.get('table_asset')
        if table_asset is None:
            if page_assets or scene is not None:
                raise ValueError("Observed bank needs a table alias")
            continue
        if assets.get(table_asset) is None or assets[table_asset].read_bytes() != raw:
            raise ValueError(f"Table alias content mismatch: {table_asset}")
        if len(page_assets) != count:
            raise ValueError("Page alias count does not match bank")
        for i, alias in enumerate(page_assets):
            canonical = table.parent/f'page-{i}.hpt'
            if alias in aliases or alias not in assets:
                raise ValueError(f"Duplicate/missing page alias: {alias}")
            if assets[alias].read_bytes() != canonical.read_bytes():
                raise ValueError(f"Page alias content mismatch: {alias}")
            aliases[alias] = (name, i)
    for alias in spec['unchanged_assets']:
        if alias in aliases or alias not in assets:
            raise ValueError(f"Duplicate/missing unchanged alias: {alias}")
        page_report.read_hpt(assets[alias])
        aliases[alias] = ('untouched', str(assets[alias]))
    return tables, aliases, rooms, assets


def usage(trace, tables, aliases, rooms, assets):
    """Stream exact rectangles; disambiguate reused page IDs by route bank."""
    mapping, bank_scenes = {}, {}
    for row in page_report.rows(trace/'page-banks.csv'):
        alias = row['asset']
        if alias.startswith('static-room-page-') and row['scene'] != 'Tutorial_01':
            alias = f"rooms/{row['scene']}/{alias}"
        key = int(row['bank']), int(row['page'])
        if key[0] in bank_scenes and bank_scenes[key[0]] != row['scene']:
            raise ValueError("Bank maps to multiple scenes")
        bank_scenes[key[0]] = row['scene']
        if key in mapping or alias not in aliases:
            raise ValueError(f"Duplicate/unknown bank page: {key}, {alias}")
        owner, page = aliases[alias]
        if owner != 'untouched' and tables[owner]['scene'] not in (None, row['scene']):
            raise ValueError("Room page belongs to another scene")
        mapping[key] = (row['scene'], owner, page, page_report.read_hpt(assets[alias]))
    lookup = {}
    for name, bank in tables.items():
        for i, s in enumerate(bank['sprites']):
            page, x, y, w, h = s[:5]
            lookup.setdefault((name, page, x, y, x+w, y+h), set()).add((name, i))
    rows = page_report.rows(trace/'page-trace.csv')
    cameras = page_report.rows(trace/'metrics.csv')
    if not rows or [int(r['frame']) for r in rows] != list(range(len(rows))):
        raise ValueError("Page frames must be contiguous from zero")
    if len(cameras) != len(rows) or any(int(c['frame']) != i or c['scene'] != rows[i]['scene']
                                      for i, c in enumerate(cameras)):
        raise ValueError("Camera/page frame or scene mismatch")
    frames, originals = [], []
    with (trace/'sprite-trace.csv').open(newline='', encoding='utf-8-sig') as stream:
        iterator = iter(csv.DictReader(stream))
        pending = next(iterator, None)
        for index, row in enumerate(rows):
            scene, bank = row['scene'], int(row['bank'])
            if bank_scenes.get(bank) != scene:
                raise ValueError("Page bank/scene mismatch")
            if scene not in rooms:
                raise ValueError(f"Missing scene bank: {scene}")
            seen, ids, source = set(), set(), set()
            while pending is not None and int(pending['frame']) == index:
                r = pending
                page = int(r['page'])
                key = bank, page
                if int(r['bank']) != bank or r['scene'] != scene or key not in mapping:
                    raise ValueError("Sprite bank/page/scene mismatch")
                mapped_scene, owner, original, texture = mapping[key]
                if mapped_scene != scene:
                    raise ValueError("Bank scene mismatch")
                uv = [float(r[k]) for k in ('u0', 'v0', 'u1', 'v1')]
                if not all(math.isfinite(v) and v == int(v) for v in uv):
                    raise ValueError("Expected integral finite UV edges")
                u, v, s, t = map(int, uv)
                x, y, right, bottom = min(u, s), min(v, t), max(u, s), max(v, t)
                if not (0 <= x < right <= texture['width'] and 0 <= y < bottom <= texture['height']):
                    raise ValueError("Invalid sprite rectangle")
                if owner == 'untouched':
                    ids.add((owner, original))
                else:
                    hits = lookup.get((owner, original, x, y, right, bottom))
                    if hits is None:
                        raise ValueError("Unmatched sprite in converted bank")
                    ids.update(hits)
                seen.add(page)
                source.add((owner, original))
                pending = next(iterator, None)
            if seen != {int(p) for p in row['pages'].split(';') if p}:
                raise ValueError("Sprite/page trace mismatch")
            frames.append(ids)
            originals.append(source)
        if pending is not None:
            raise ValueError("Out-of-order or extra sprite frame")
    return rows, cameras, frames, originals


def analyze_serialized(trace, manifest, candidates, margin=64):
    """Audit candidate bytes and report demand without running the packer.

    candidates is JSON: banks=[{name: source owner, table: candidate path}].
    Paths resolve relative to that JSON. Shared tables in the same directory
    use one page/CLUT namespace. Room directories must remain separate.
    Every physical sprite, including unused sprites, is audited against its
    source. Used/unused packing policy remains the exporter's responsibility.
    """
    if not math.isfinite(margin) or margin < 0:
        raise ValueError("Window margin must be finite and nonnegative")
    trace, manifest, candidates = Path(trace), Path(manifest), Path(candidates)
    tables, aliases, rooms, assets = load_banks(trace, manifest)
    rows, cameras, frames, originals = usage(trace, tables, aliases, rooms, assets)
    entries = {}
    for entry in json.loads(candidates.read_text())['banks']:
        name = entry['name']
        if name not in tables or name in entries:
            raise ValueError(f"Unknown/duplicate candidate owner: {name}")
        entries[name] = (candidates.parent/entry['table']).resolve()
    needed = {n for frame in frames for n, _ in frame if n != 'untouched'} | set(rooms.values())
    if needed-set(entries):
        raise ValueError(f"Missing candidate owners: {sorted(needed-set(entries))}")
    namespaces = {}
    for name, table in entries.items():
        namespaces.setdefault(table.parent, []).append(name)
    for owners in namespaces.values():
        if len(owners) > 1 and any(tables[n]['scene'] is not None for n in owners):
            raise ValueError("Room candidate must have a separate page namespace")
    Image = pack.room.import_pil()
    payloads, remap, audits = {}, {}, []
    for name, table in entries.items():
        source = tables[name]
        raw = table.read_bytes()
        actual = pack.read_layout_bank(raw, with_cluts=True)
        if (actual['source_version'] != source['source_version'] or
                actual['logical_parts'] != source['logical_parts'] or
                actual['logical_regions'] != source['logical_regions'] or
                [s[3:] for s in actual['logical_sprites']] != [s[3:] for s in source['logical_sprites']] or
                raw[actual['end']:actual['base_end']] !=
                    source['table'].read_bytes()[source['end']:source['base_end']]):
            raise ValueError(f"Candidate logical mapping, geometry or tail changed: {name}")
        directory = table.parent
        if directory not in payloads:
            pcl = (directory/'palettes.pcl').read_bytes()
            if len(pcl) < 12:
                raise ValueError("Truncated candidate palette bank")
            magic, version, count = struct.unpack_from('<4sII', pcl)
            if magic != b'PCL1' or version != 1 or not 1 <= count <= 128 or len(pcl) != 12+1024*count:
                raise ValueError("Invalid candidate palette bank")
            palettes = [pcl[12+1024*i:12+1024*(i+1)] for i in range(count)]
            indices = []
            header = struct.pack('<4sHH4BI', b'HPT2', 512, 512, 5, 3, 1, 0, count)
            for i in range(actual['count']):
                page = (directory/f'page-{i}.hpt').read_bytes()
                if page[:16] != header or len(page) != 16+512*512:
                    raise ValueError("Invalid candidate page header/size")
                indices.append(pack.room.unswizzle(page[16:], 512, 512))
            payloads[directory] = indices, palettes
        indices, palettes = payloads[directory]
        if actual['count'] != len(indices):
            raise ValueError("Candidate namespace page count mismatch")
        plan = dict(size=(512, 512), sprites=actual['sprites'], cluts=actual['cluts'],
                    indices=indices, palettes=palettes, groups=['audit']*len(indices))
        pack.verify_mixed(pack.read_pages(Image, source['table'].parent, source['count']),
                          source['sprites'], ['audit']*len(source['sprites']), plan)
        for i, sprite in enumerate(actual['sprites']):
            remap[name, i] = (str(directory), sprite[0])
        audits.append(dict(bank=name, table=str(table), table_sha256=digest(table),
            physical_sprites=len(actual['sprites']), logical_sprites=len(actual['logical_sprites']),
            source_version=actual['source_version'], before_pages=source['count'], after_pages=actual['count']))
    measurements = []
    for scene, name in rooms.items():
        visits = [i for i, row in enumerate(rows) if row['scene'] == scene]
        if not visits:
            raise ValueError(f"Unobserved scene bank: {scene}")
        bank = tables[name]
        logical = fixed.camera_footprints([set() for _ in visits], [cameras[i] for i in visits],
            name, dict(bank, sprites=bank['logical_sprites']), margin)
        windows = [{part for sprite in window for part in bank['logical_parts'][sprite]} |
            {i for owner, i in frames[frame] if owner == name} for frame, window in zip(visits, logical)]
        pinned = {(n, i) if n == 'untouched' else remap[n, i]
            for frame in visits for n, i in frames[frame] if n != name}
        counts = [len({remap[name, i] for i in window}) for window in windows]
        old_pinned = {p for frame in visits for p in originals[frame] if p[0] != name}
        old_window = max(len({bank['sprites'][i][0] for i in window}) for window in windows)
        total = len(pinned)+max(counts)
        measurements.append(dict(scene=scene, frames=len(visits),
            before=dict(observed_pinned_pages=len(old_pinned), max_static_grown_window_pages=old_window,
                        pinned_plus_static_window=len(old_pinned)+old_window),
            after=dict(observed_pinned_pages=len(pinned), max_static_grown_window_pages=max(counts),
                       pinned_plus_static_window=total, fits_21_slots=total <= 21, pool_bytes=total*265216,
                       witness_frame=visits[counts.index(max(counts))]),
            window_pages_per_frame=list(zip(visits, counts))))
    packages = []
    for directory in payloads:
        files = {p.relative_to(directory).as_posix(): dict(bytes=p.stat().st_size, sha256=digest(p))
                 for p in sorted(directory.rglob('*')) if p.is_file()}
        packages.append(dict(directory=str(directory), files=files,
            bytes_excluding_audit=sum(v['bytes'] for n, v in files.items() if n != 'repack.json')))
    return dict(frames=len(frames), margin_pixels=margin, rooms=measurements, banks=audits, packages=packages,
        manifest_sha256=digest(manifest), candidate_manifest_sha256=digest(candidates),
        trace_sha256={n: digest(trace/n) for n in ('assets.tsv', 'page-banks.csv', 'page-trace.csv',
                                                 'sprite-trace.csv', 'metrics.csv')},
        limitations=['Observed visits only; incomplete first-ending route',
            'Serialized candidate demand; no runtime overflow, raster parity or measured heap acceptance',
            'Candidate package bytes exclude unchanged shipping banks/assets',
            'Slot bytes exclude runtime palette, sprite, quad and cache allocations'])


def analyze(trace, manifest, margin=64):
    if not math.isfinite(margin) or margin < 0:
        raise ValueError("Window margin must be finite and nonnegative")
    trace, manifest = Path(trace), Path(manifest)
    tables, aliases, rooms, assets = load_banks(trace, manifest)
    rows, cameras, frames, originals = usage(trace, tables, aliases, rooms, assets)
    indices = {scene: [i for i, r in enumerate(rows) if r['scene'] == scene] for scene in rooms}
    footprints = {}
    for scene, visits in indices.items():
        name, bank = rooms[scene], tables[rooms[scene]]
        logical_windows = fixed.camera_footprints([set() for _ in visits],
            [cameras[i] for i in visits], name, dict(bank, sprites=bank['logical_sprites']), margin)
        footprints[name] = [frozenset({part for logical in window for part in bank['logical_parts'][logical]} |
            {i for n, i in frames[frame] if n == name}) for frame, window in zip(visits, logical_windows)]
    used = set().union(*frames)
    Image = pack.room.import_pil()
    remap, reports = {}, []
    total_indices = total_palettes = 0
    for name, bank in tables.items():
        sprites = bank['sprites']
        active = {i for i in range(len(sprites)) if (name, i) in used}
        groups = ['used' if i in active else 'unused' for i in range(len(sprites))]
        windows = footprints.get(name)
        if bank['scene'] is None and active:
            # One persistent shared bank: co-pack each room's entire observed
            # owner union, never independently repack it for each room.
            windows = [frozenset(i for frame in visits for owner, i in frames[frame] if owner == name)
                       for visits in indices.values()]
        pages = pack.read_pages(Image, bank['table'].parent, bank['count'])
        plan = pack.pack_mixed(pages, sprites, groups, windows,
                               optimize_windows=bank['scene'] is not None)
        for i, sprite in enumerate(plan['sprites']):
            remap[name, i] = (name, sprite[0])
        index_bytes = sum(map(len, plan['indices']))
        palette_bytes = sum(map(len, plan['palettes']))
        total_indices += index_bytes
        total_palettes += palette_bytes
        reports.append(dict(bank=name, scene=bank['scene'], sprites=len(sprites),
            source_version=bank['source_version'], logical_sprites=bank['logical_sprites'],
            logical_parts=bank['logical_parts'], logical_regions=bank['logical_regions'],
            used_sprites=len(active), before_pages=bank['count'], after_pages=len(plan['indices']),
            moved_sprites=sum(a[:3] != b[:3] for a, b in zip(sprites, plan['sprites'])),
            profile=dict(table_sha256=digest(bank['table']), groups=groups,
                page_sha256=[digest(bank['table'].parent/f'page-{i}.hpt') for i in range(bank['count'])],
                footprint_sha256=hashlib.sha256(json.dumps([sorted(w) for w in windows],
                    separators=(',', ':')).encode()).hexdigest() if windows is not None else None,
                algorithm=plan['algorithm'] if bank['scene'] is not None else
                    ('first-room-use-area-maxrects' if windows is not None else 'shelf')),
            sprite_records=plan['sprites'], sprite_cluts=plan['cluts'], page_groups=plan['groups'],
            index_sha256=[hashlib.sha256(p).hexdigest() for p in plan['indices']],
            clut_sha256=[hashlib.sha256(p).hexdigest() for p in plan['palettes']],
            index_bytes=index_bytes, palette_bytes=palette_bytes))
    measurements = []
    for scene, visits in indices.items():
        if not visits:
            raise ValueError(f"Unobserved scene bank: {scene}")
        name = rooms[scene]
        old_pinned = set().union(*(originals[i] for i in visits))
        old_pinned = {p for p in old_pinned if p[0] != name}
        pinned = {remap[n, i] if n != 'untouched' else (n, i)
                  for frame in visits for n, i in frames[frame] if n != name}
        old_window = max(len({tables[name]['sprites'][i][0] for i in w}) for w in footprints[name])
        counts = [len({remap[name, i] for i in w}) for w in footprints[name]]
        maximum = max(counts)
        total = len(pinned)+maximum
        measurements.append(dict(scene=scene, frames=len(visits),
            before=dict(observed_pinned_pages=len(old_pinned), max_static_grown_window_pages=old_window,
                        pinned_plus_static_window=len(old_pinned)+old_window),
            after=dict(observed_pinned_pages=len(pinned), max_static_grown_window_pages=maximum,
                       pinned_plus_static_window=total, witness_frame=visits[counts.index(maximum)],
                       fits_21_slots=total <= 21, pool_bytes=total*265216,
                       extra_pool_bytes_over_21=max(0, total-21)*265216),
            window_pages_per_frame=list(zip(visits, counts))))
    return dict(frames=len(frames), margin_pixels=margin, rooms=measurements, banks=reports,
        trace_sha256={n: digest(trace/n) for n in ('assets.tsv', 'page-banks.csv', 'page-trace.csv',
                                                   'sprite-trace.csv', 'metrics.csv')},
        manifest_sha256=digest(manifest),
        offline_payload=dict(indices_bytes=total_indices, palettes_bytes=total_palettes,
                             total_bytes=total_indices+total_palettes),
        limitations=['Observed visits only; not proof of the complete first-ending route',
                     'Sufficient slot counts for this layout, not proven minimum slot counts',
                     'Offline index/CLUT payload excludes shipping metadata and runtime heap'])
