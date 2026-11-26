"""Read-only page residency analysis of real host draw-list tapes.

Build with host-build.ps1 -PageTrace, then pass --host to replay (no raster).
Alternatively --trace consumes an existing trace directory, including a future
combat replay produced by the same host trace interface. No game data is written.
Page costs match backend_psp.cpp Page::bytes, including all three T8 palettes.
"""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import subprocess
import struct

ROOT = Path(__file__).resolve().parents[2]
MIB = 1024 * 1024


def rows(path):
    with Path(path).open(newline="", encoding="utf-8-sig") as stream:
        return list(csv.DictReader(stream))


def read_hpt(path):
    """Validate the complete indexed texture, retaining the exact source CLUT."""
    raw = Path(path).read_bytes()
    if len(raw) < 16 or raw[:4] != b"HPT1":
        raise ValueError("Invalid route HPT1")
    w, h, psm, clut, swizzle, reserved, count = struct.unpack_from("<HHBBBBI", raw, 4)
    entries = 16 if psm == 4 else 256
    if (psm not in (4, 5) or (clut, swizzle, reserved, count) != (3, 1, 0, entries) or
            not 16 <= w <= 512 or not 8 <= h <= 512 or w & (w-1) or h & (h-1) or
            len(raw) != 16+4*entries+w*h//(2 if psm == 4 else 1)):
        raise ValueError("Invalid route HPT1 layout")
    return dict(width=w, height=h, bits=4 if psm == 4 else 8,
                palette=raw[16:16+4*entries], sha256=hashlib.sha256(raw).hexdigest())


def analyze_sprite_bytes(trace):
    """Ideal area bounds for intact source rectangles, not an achieved packing.

    Page membership cannot identify a drawn sprite. Require the UV trace and
    cross-check its page set against EVERY frame of the existing page trace.
    Repeated draws of the same rectangle count once. No cross-sprite dedup is
    assumed; different rectangles must remain separately addressable.
    """
    trace = Path(trace)
    if not (trace/"sprite-trace.csv").is_file():
        raise ValueError("Missing sprite-trace.csv: page-only traces cannot measure visible rectangles")
    assets = dict(line.split("\t", 1) for line in (trace/"assets.tsv").read_text().splitlines())
    bank_file = trace/"page-banks.csv"
    if bank_file.is_file():
        mapping = rows(bank_file)
    else:
        names = [name for name in assets if name.endswith(".hpt")]
        sizes = rows(trace/"page-sizes.csv")
        if len(names) != len(sizes) or [int(r["page"]) for r in sizes] != list(range(len(names))):
            raise ValueError("Texture order/count does not match page sizes")
        mapping = [dict(bank="0", scene="Tutorial_01", page=str(i), asset=name)
                   for i, name in enumerate(names)]
    banks, textures = {}, {}
    for row in mapping:
        name = row["asset"]
        if name.startswith("static-room-page-") and row["scene"] != "Tutorial_01":
            name = f"rooms/{row['scene']}/{name}"
        path = Path(assets[name])
        if path not in textures:
            textures[path] = read_hpt(path)
        key = int(row["bank"]), int(row["page"])
        if key in banks:
            raise ValueError("Duplicate route bank page")
        banks[key] = dict(textures[path], asset=name, scene=row["scene"])
    frames = rows(trace/"page-trace.csv")
    if not frames or [int(r["frame"]) for r in frames] != list(range(len(frames))):
        raise ValueError("Route trace frames must be contiguous from zero")
    # Keep only one frame's rectangles in memory; real route traces are large.
    def rectangles():
        with (trace/"sprite-trace.csv").open(newline="", encoding="utf-8-sig") as stream:
            yield from csv.DictReader(stream)
    iterator = iter(rectangles())
    pending = next(iterator, None)
    rooms, measurements = {}, []
    for frame in frames:
        index, bank = int(frame["frame"]), int(frame.get("bank", 0))
        scene = frame.get("scene", "Tutorial_01")
        required = {int(p) for p in frame["pages"].split(";") if p}
        rects, palettes, seen = {}, set(), set()
        while pending is not None and int(pending["frame"]) == index:
            r = pending
            page = int(r["page"])
            if int(r.get("bank", bank)) != bank or (bank, page) not in banks:
                raise ValueError("Unknown sprite bank/page")
            texture = banks[bank, page]
            if texture["scene"] != scene:
                raise ValueError("Sprite bank scene mismatch")
            uv = [float(r[k]) for k in ("u0", "v0", "u1", "v1")]
            if not all(math.isfinite(v) and v == int(v) for v in uv):
                raise ValueError("Sprite UVs must be finite integer texel edges")
            u0, v0, u1, v1 = map(int, uv)
            x, y, right, bottom = min(u0, u1), min(v0, v1), max(u0, u1), max(v0, v1)
            if not (0 <= x < right <= texture["width"] and 0 <= y < bottom <= texture["height"]):
                raise ValueError(f"Sprite rectangle outside texture at frame {index}, page {page}: "
                                 f"{uv} in {texture['width']}x{texture['height']}")
            # Replicated 1px filtering border, clipped for full-page textures.
            width = min(right+1, texture["width"])-max(0, x-1)
            height = min(bottom+1, texture["height"])-max(0, y-1)
            rects[texture["asset"], x, y, right, bottom] = (width*height*texture["bits"]+7)//8
            palettes.add(texture["palette"])
            seen.add(page)
            pending = next(iterator, None)
        if seen != required:
            raise ValueError(f"Sprite/page trace mismatch at frame {index}")
        size, clut = sum(rects.values()), sum(map(len, palettes))
        # 265216 B is the existing physical slot; indices have 262144 B capacity.
        memory_bound = (size+clut+265215)//265216
        texture_bound = (size+262143)//262144
        bound = max(memory_bound, texture_bound)
        measurements.append(dict(frame=index, bank=bank, scene=scene,
                                 padded_rectangle_bytes=size, clut_bytes=clut,
                                 distinct_cluts=len(palettes), ideal_slots_lower_bound=bound))
        result = rooms.setdefault(scene, dict(scene=scene, frames=0, max_padded_rectangle_bytes=0,
                                  max_clut_bytes=0, max_distinct_cluts=0, ideal_slots_lower_bound=0,
                                  witness_frame=None, witness_rectangle_bytes=0, witness_clut_bytes=0))
        result["frames"] += 1
        result["max_padded_rectangle_bytes"] = max(result["max_padded_rectangle_bytes"], size)
        result["max_clut_bytes"] = max(result["max_clut_bytes"], clut)
        result["max_distinct_cluts"] = max(result["max_distinct_cluts"], len(palettes))
        if bound > result["ideal_slots_lower_bound"]:
            result.update(ideal_slots_lower_bound=bound, witness_frame=index,
                          witness_rectangle_bytes=size, witness_clut_bytes=clut)
    if pending is not None:
        raise ValueError("Sprite trace has unordered or extra frames")
    inputs = [trace/"assets.tsv", trace/"page-trace.csv", trace/"sprite-trace.csv",
              bank_file if bank_file.is_file() else trace/"page-sizes.csv"]
    return dict(rooms=list(rooms.values()), frame_measurements=measurements,
                frames=len(frames), slot_bytes=265216,
                basis="M: distinct intact source UV rectangles, 1px padded; CLUT equality by bytes",
                spatial_margin=None, grown_window_pages=None,
                limitations=["Ideal area bound excludes packing waste, pinned libraries and prefetch",
                             "No cross-sprite deduplication; not a measured sufficient slot count",
                             "CLUT bytes count originals once, excluding runtime derived CLUTs"],
                inputs={p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs},
                textures={str(path): t["sha256"] for path, t in textures.items()})


def write_sprite_byte_report(report, output):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    (output/"sprite-bytes.json").write_text(json.dumps(report, indent=2)+"\n", encoding="utf-8")
    with (output/"sprite-bytes.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(report["rooms"][0]))
        writer.writeheader()
        writer.writerows(report["rooms"])
    with (output/"sprite-frames.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(report["frame_measurements"][0]))
        writer.writeheader()
        writer.writerows(report["frame_measurements"])


def replay_cache(frames, sizes, target=3 * MIB):
    """Exact original preparePages policy: mark, evict LRU absent, load by ID."""
    resident, last = set(), {}
    result = []
    for frame, required in enumerate(frames):
        if not required <= sizes.keys():
            raise ValueError("Unknown page in draw list")
        last.update(dict.fromkeys(required, frame))
        wanted = sum(sizes[p] for p in resident | required)
        evicted = []
        while wanted > target and resident - required:
            page = min(resident - required, key=lambda p: (last[p], p))
            resident.remove(page)
            evicted.append(page)
            wanted -= sizes[page]
        loaded = sorted(required - resident)
        resident |= required
        result.append({"frame": frame, "pages": sorted(required),
                       "loaded": loaded, "evicted": evicted,
                       "required_bytes": sum(sizes[p] for p in required),
                       "resident_bytes": sum(sizes[p] for p in resident)})
    return result


def intervals(frames, page, lookahead):
    """Future demand windows, merged when look-ahead overlaps an earlier use."""
    windows = []
    for frame, required in enumerate(frames):
        if page not in required:
            continue
        start = max(0, frame - lookahead)
        if windows and start <= windows[-1][1] + 1:
            windows[-1][1] = frame
        else:
            windows.append([start, frame])
    return windows


def analyze_route_palettes(trace):
    """Measured slot lower bounds, independent of packing and prefetch policy.

    A frame using N distinct unchanged CLUTs needs at least N physical pages.
    This is a lower bound, not an achievable packing or a grown camera window.
    Read the traced HPTs themselves; never infer palette equality from names.
    """
    trace = Path(trace)
    assets = dict(line.split("\t", 1) for line in (trace/"assets.tsv").read_text().splitlines())
    banks, fingerprints = {}, {}
    for row in rows(trace/"page-banks.csv"):
        bank, page = int(row["bank"]), int(row["page"])
        name = row["asset"]
        if name.startswith("static-room-page-") and row["scene"] != "Tutorial_01":
            name = f"rooms/{row['scene']}/{name}"
        path = Path(assets[name])
        if path not in fingerprints:
            texture = read_hpt(path)
            fingerprints[path] = (texture["palette"], texture["sha256"])
        palette, digest = fingerprints[path]
        table = banks.setdefault(bank, {})
        if page in table or int(row["bytes"]) <= 0:
            raise ValueError("Invalid route bank page")
        table[page] = dict(scene=row["scene"], asset=name, palette=palette,
                           bytes=int(row["bytes"]), sha256=digest)
    rooms = {}
    frames = rows(trace/"page-trace.csv")
    if not frames or [int(r["frame"]) for r in frames] != list(range(len(frames))):
        raise ValueError("Route trace frames must be contiguous from zero")
    for row in frames:
        bank, scene = int(row["bank"]), row["scene"]
        required = {int(p) for p in row["pages"].split(";") if p}
        if bank not in banks or not required <= banks[bank].keys():
            raise ValueError("Unknown route page")
        if any(p["scene"] != scene for p in banks[bank].values()):
            raise ValueError("Route bank scene mismatch")
        pages = [banks[bank][p] for p in required]
        result = rooms.setdefault(scene, dict(scene=scene, frames=0, max_visible_pages=0,
                                  palette_slots_lower_bound=0, witness_frame=None,
                                  witness_assets=[], observed_shared_assets=set(),
                                  observed_room_assets=set(), max_visible_room_pages=0))
        result["frames"] += 1
        result["max_visible_pages"] = max(result["max_visible_pages"], len(pages))
        room_pages = [p for p in pages if Path(p["asset"]).name.startswith("static-room-page-")]
        shared = [p for p in pages if p not in room_pages]
        result["observed_shared_assets"].update(p["asset"] for p in shared)
        result["observed_room_assets"].update(p["asset"] for p in room_pages)
        result["max_visible_room_pages"] = max(result["max_visible_room_pages"], len(room_pages))
        distinct = len({p["palette"] for p in pages})
        if distinct > result["palette_slots_lower_bound"]:
            result.update(palette_slots_lower_bound=distinct, witness_frame=int(row["frame"]),
                          witness_assets=sorted(p["asset"] for p in pages))
    for result in rooms.values():
        for key in ("observed_shared_assets", "observed_room_assets"):
            result[key] = sorted(result[key])
        result["fits_21_ruled_out"] = result["palette_slots_lower_bound"] > 21
    return dict(rooms=list(rooms.values()), frames=len(frames),
                basis="M: distinct unchanged CLUTs in each observed frame; one CLUT per page",
                spatial_margin=None, grown_window_pages=None,
                limitations=["Lower bounds exclude packing waste, unseen actions and prefetch",
                             "Observed shared union is tape-only, not a complete pinned library",
                             "Visible room pages are not a spatially grown camera window"],
                inputs={p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                        for p in (trace/"assets.tsv", trace/"page-banks.csv", trace/"page-trace.csv")},
                textures={str(path): digest for path, (palette, digest) in fingerprints.items()})


def write_route_palette_report(report, output):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    (output/"route-palettes.json").write_text(json.dumps(report, indent=2)+"\n", encoding="utf-8")
    fields = ["scene", "frames", "max_visible_pages", "palette_slots_lower_bound",
              "max_visible_room_pages", "observed_shared_pages", "observed_room_pages",
              "witness_frame", "fits_21_ruled_out"]
    with (output/"route-palettes.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for room in report["rooms"]:
            row = {k: room[k] for k in fields if k in room}
            row.update(observed_shared_pages=len(room["observed_shared_assets"]),
                       observed_room_pages=len(room["observed_room_assets"]))
            writer.writerow(row)


def analyze(trace, perf=None, cpu=None, memory=None, lookahead=30):
    trace = Path(trace)
    sizes_rows = rows(trace / "page-sizes.csv")
    sizes = {int(r["page"]): int(r["bytes"]) for r in sizes_rows}
    if len(sizes) != len(sizes_rows) or sorted(sizes) != list(range(len(sizes))) or any(b <= 0 for b in sizes.values()):
        raise ValueError("Invalid page size table")
    names = [line.split("\t")[0] for line in (trace / "assets.tsv").read_text().splitlines()
             if line.split("\t")[0].endswith(".hpt")]
    if len(names) != len(sizes):
        raise ValueError("Texture asset order/count differs from bank")
    raw = rows(trace / "page-trace.csv")
    if not raw or [int(r["frame"]) for r in raw] != list(range(len(raw))):
        raise ValueError("Trace frames must be contiguous from zero")
    frames = [{int(p) for p in r["pages"].split(";") if p} for r in raw]
    timeline = replay_cache(frames, sizes)
    observed = set().union(*frames)
    room = {p for p, n in enumerate(names) if n.startswith("static-room-page-")}
    # Conservative whole libraries: hero, HUD, fonts, actors and shared effects.
    # Ability UI and shade remain included: their exclusion needs a room contract.
    pinned = set(sizes) - room
    pinned_bytes = sum(sizes[p] for p in pinned)
    max_required = max(t["required_bytes"] for t in timeline)
    windows = []
    for frame in range(len(frames)):
        pages = set().union(*frames[frame:frame + lookahead + 1]) & room
        windows.append(pages)
        timeline[frame]["room_lookahead_pages"] = sorted(pages)
        timeline[frame]["room_lookahead_bytes"] = sum(sizes[p] for p in pages)
    window_bytes = max(sum(sizes[p] for p in w) for w in windows)
    stalls, mismatches = [], []
    if perf or cpu:
        if not perf or not cpu:
            raise ValueError("Cross-check needs both perf.csv and cpu.csv")
        hw = rows(perf)
        baseline = [r for r in rows(cpu) if r["config"] == "baseline"]
        metrics = rows(trace / "metrics.csv")
        if not (len(hw) == len(baseline) == len(metrics) == len(frames)):
            raise ValueError("Hardware/replay frame count differs")
        for f, (p, c, m) in enumerate(zip(hw, baseline, metrics)):
            if int(p["frame"]) != f or int(c["frame"]) != f:
                raise ValueError("Hardware frames out of order")
            if int(m["quads"]) != int(p["quads"]) or int(c["quads"]) != int(p["quads"]):
                mismatches.append(f)
            # Run-2 section 4: non-startup synchronous uploads above 30 ms.
            if f and int(c["upload_us"]) > 30000:
                loaded = timeline[f]["loaded"]
                stalls.append({"frame": f, "upload_us": int(c["upload_us"]),
                               "gu_us": int(p["gu_us"]), "total_us": int(p["total_us"]),
                               "page_ids": loaded, "assets": [names[p] for p in loaded],
                               "bytes": sum(sizes[p] for p in loaded),
                               "attribution": "H: host demand + original cache replay; hardware has no page IDs"})
    heap = {}
    if memory:
        heap = dict(line.split("=", 1) for line in Path(memory).read_text().splitlines() if "=" in line)
    # Existing peak includes CPU texture data as well as GPU resident pages.
    # Add only the proposed residency delta; preserve every other allocation.
    peak = int(heap["peak_accounted_heap_bytes"]) if heap else None
    old_resident = max(t["resident_bytes"] for t in timeline)
    proposed = pinned_bytes + window_bytes
    estimated_heap = peak + proposed - old_resident if peak is not None else None
    observed_bytes = sum(sizes[p] for p in observed)
    tape_heap = peak + observed_bytes - old_resident if peak is not None else None
    # Sum category high waters to bound overlap of non-texture owners. The
    # replacement of cached texture peak is still a host-model assumption [H].
    high_keys = ["textures_high_water_bytes", "audio_high_water_bytes",
                 "room_banks_high_water_bytes", "draw_list_scratch_high_water_bytes",
                 "other_high_water_bytes"]
    high_base = sum(int(heap[k]) for k in high_keys) - old_resident if all(k in heap for k in high_keys) else None
    page_table = [{"page": p, "asset": names[p], "bytes": sizes[p],
                   "observed": p in observed, "pinned": p in pinned,
                   "lookahead_windows": intervals(frames, p, lookahead)} for p in sizes]
    predicted_misses = [t["frame"] for t in timeline if t["frame"] and t["loaded"]]
    hardware_misses = [s["frame"] for s in stalls]
    events = {}
    for label, prefixes in (("nail_slash_and_hero_hit", ("page-",)),
                            ("hit_and_decor_burst", ("decor-fx-page-",)),
                            ("pickup", ("geo-page-",))):
        ids = [p for p, name in enumerate(names) if name.startswith(prefixes)]
        events[label] = {"candidate_page_ids": ids,
                         "observed_page_ids": sorted(set(ids) & observed),
                         "windows_by_page": {p: intervals(frames, p, lookahead) for p in ids},
                         "basis": "H: complete library candidate; tape has no semantic event markers"}
    return {"room": "Tutorial_01", "frames": len(frames), "lookahead_frames": lookahead,
            "lookahead_ms_at_30fps": lookahead * 1000 / 30,
            "pages": page_table, "timeline": timeline, "stalls": stalls,
            "hardware_quad_mismatch_frames": mismatches,
            "predicted_nonstartup_miss_frames": predicted_misses,
            "hardware_stall_frame_symmetric_difference": sorted(set(predicted_misses) ^ set(hardware_misses)) if perf else None,
            "observed_union_bytes": observed_bytes,
            "tape_only_pinned_pages": sorted(observed),
            "tape_only_pinned_bytes": observed_bytes,
            "tape_only_estimated_heap_bytes": tape_heap,
            "tape_only_high_water_heap_estimate_bytes": high_base + observed_bytes if high_base is not None else None,
            "tape_only_high_water_estimate_fits": high_base + observed_bytes <= 16 * MIB if high_base is not None else None,
            "tape_only_scope": "H: load entire observed union at room start; covers this tape only, not unseen actions",
            "minimum_simultaneous_pages": max(timeline, key=lambda t: t["required_bytes"])["pages"],
            "minimum_simultaneous_page_bytes": max_required,
            "minimum_high_water_heap_estimate_bytes": high_base + max_required if high_base is not None else None,
            "page_budget_from_high_water_estimate_bytes": 16 * MIB - high_base if high_base is not None else None,
            "minimum_note": "M: lower bound for this tape, excluding prefetch and other heap owners",
            "pinned_pages": sorted(pinned), "pinned_bytes": pinned_bytes,
            "max_room_lookahead_bytes": window_bytes, "proposed_resident_bytes": proposed,
            "original_cache_peak_bytes": old_resident,
            "heap_limit_bytes": 16 * MIB, "measured_heap_peak_bytes": peak,
            "estimated_heap_bytes": estimated_heap,
            "high_water_heap_estimate_bytes": high_base + proposed if high_base is not None else None,
            "estimated_heap_fits": estimated_heap <= 16 * MIB if estimated_heap is not None else None,
            "event_candidates": events,
            "limitations": ["H: bank labels are event candidates, not semantic event markers",
                            "Only observed tape demand is measured; unseen events need combat tape",
                            "Look-ahead windows are offline demand, not loader deadlines or camera margins",
                            "H: heap estimate replaces peak cached bytes; allocation peaks may not coincide",
                            "Arena fragmentation and loader/audio contention require real PSP acceptance"],
            "inputs": {str(p): hashlib.sha256(Path(p).read_bytes()).hexdigest()
                       for p in [trace / "page-trace.csv", trace / "page-sizes.csv", trace / "assets.tsv",
                                 *([trace / "metrics.csv"] if (trace / "metrics.csv").is_file() else []),
                                 *([perf, cpu] if perf else []), *([memory] if memory else [])]}}


def write_report(report, output):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    for name, records in (("frames", report["timeline"]), ("stalls", report["stalls"]), ("pages", report["pages"])):
        with (output / (name + ".csv")).open("w", newline="") as stream:
            if not records:
                continue
            writer = csv.DictWriter(stream, fieldnames=list(records[0]))
            writer.writeheader()
            for record in records:
                writer.writerow({k: json.dumps(v) if isinstance(v, (list, dict)) else v for k, v in record.items()})


def route_demand(trace, pool_slots=21):
    """Measure complete frame unions, retaining bank identity when IDs are reused."""
    trace = Path(trace)
    banks = {}
    for row in rows(trace / "page-banks.csv"):
        bank, page, size = (int(row[k]) for k in ("bank", "page", "bytes"))
        scene, pages = banks.setdefault(bank, (row["scene"], {}))
        if bank < 0 or page < 0 or size <= 0 or page in pages or scene != row["scene"]:
            raise ValueError("Invalid route page bank")
        pages[page] = {"asset": row["asset"], "bytes": size}
    raw = rows(trace / "page-trace.csv")
    metrics = rows(trace / "metrics.csv")
    if not raw or [int(r["frame"]) for r in raw] != list(range(len(raw))):
        raise ValueError("Trace frames must be contiguous from zero")
    if len(metrics) != len(raw) or any(r["frame"] != m["frame"] or r["scene"] != m["scene"]
                                       for r, m in zip(raw, metrics)):
        raise ValueError("Route trace/metrics frame or scene mismatch")
    visits, used_banks = [], []
    for row in raw:
        frame, bank = int(row["frame"]), int(row["bank"])
        if bank not in banks or banks[bank][0] != row["scene"]:
            raise ValueError("Unknown route bank/scene")
        pages = banks[bank][1]
        ids = [int(p) for p in row["pages"].split(";") if p]
        if len(ids) != len(set(ids)) or not set(ids) <= pages.keys():
            raise ValueError("Duplicate or unknown route page")
        if not visits or visits[-1]["visit"] != bank:
            if bank != len(visits):
                raise ValueError("Route banks must be contiguous visits from zero")
            used_banks.append(bank)
            # Conservative libraries, not a proposed runtime pinning policy.
            pinned = sorted(p for p, item in pages.items()
                            if not Path(item["asset"]).name.startswith("static-room-page-"))
            visits.append(dict(visit=bank, scene=row["scene"], first_frame=frame,
                               last_frame=frame, frames=0, max_simultaneous_pages=-1,
                               max_required_bytes=-1, frames_over_pool_slots=0,
                               pinned_candidate_ids=pinned, pinned_candidate_pages=len(pinned),
                               pinned_candidate_bytes=sum(pages[p]["bytes"] for p in pinned),
                               observed_ids=set()))
        visit = visits[-1]
        visit["frames"] += 1
        visit["last_frame"] = frame
        visit["observed_ids"].update(ids)
        required_bytes = sum(pages[p]["bytes"] for p in ids)
        if len(ids) > visit["max_simultaneous_pages"]:
            visit.update(max_simultaneous_pages=len(ids), max_pages_frame=frame,
                         max_pages_ids=sorted(ids), max_pages_bytes=required_bytes)
        if required_bytes > visit["max_required_bytes"]:
            visit.update(max_required_bytes=required_bytes, max_bytes_frame=frame,
                         max_bytes_ids=sorted(ids))
        if len(ids) > pool_slots:
            visit["frames_over_pool_slots"] += 1
    if set(used_banks) != banks.keys():
        raise ValueError("Unreferenced route page bank")
    slot_bytes = max((item["bytes"] + 15) & ~15
                     for _, pages in banks.values() for item in pages.values())
    for visit in visits:
        pages = banks[visit["visit"]][1]
        observed = visit["observed_ids"]
        visit["observed_bytes"] = sum(pages[p]["bytes"] for p in observed)
        visit["observed_ids"] = sorted(observed)
        pinned = observed & set(visit["pinned_candidate_ids"])
        visit["observed_pinned_candidate_ids"] = sorted(pinned)
        visit["observed_pinned_candidate_bytes"] = sum(pages[p]["bytes"] for p in pinned)
        visit["max_required_slot_bytes"] = visit["max_simultaneous_pages"] * slot_bytes
    return dict(frames=len(raw), visits=visits, pool_slots=pool_slots, pool_slot_bytes=slot_bytes,
                limitations=["Measured host draw-list unions for this tape only; no PSP timing/heap claim",
                             "Pinned candidates are complete non-room libraries [H], not event coverage",
                             "Per-pass cumulative submission retains the complete frame union until sync"],
                inputs={str(trace / name): hashlib.sha256((trace / name).read_bytes()).hexdigest()
                        for name in ("page-trace.csv", "page-banks.csv", "metrics.csv", "assets.tsv")})


def write_route_demand(report, output):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    (output / "page-demand.json").write_text(json.dumps(report, indent=2) + "\n")
    with (output / "page-demand.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(report["visits"][0]))
        writer.writeheader()
        for visit in report["visits"]:
            writer.writerow({k: json.dumps(v) if isinstance(v, list) else v for k, v in visit.items()})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trace", type=Path, default=ROOT / "build/psp/page-report/trace")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--route-palettes", action="store_true", help="Report palette slot lower bounds for a multi-room trace")
    mode.add_argument("--sprite-bytes", action="store_true", help="Measure intact visible rectangles from sprite-trace.csv")
    mode.add_argument("--fixed-layout", action="store_true", help="Offline fixed-UV multi-CLUT grouping/window report")
    mode.add_argument("--mixed-layout", action="store_true", help="Relocating multi-CLUT grouping/window report")
    mode.add_argument("--route-mixed-layout", action="store_true", help="One relocating plan across route rooms; JSON alias manifest")
    mode.add_argument("--serialized-layout", action="store_true", help="Audit serialized multi-CLUT candidates and measure route demand")
    parser.add_argument("--candidate-manifest", type=Path, help="JSON candidate owner/table mappings for --serialized-layout")
    parser.add_argument("--bank-manifest", type=Path, help="Baseline bank TSV; JSON aliases for --route-mixed-layout")
    parser.add_argument("--cell-size", type=float, default=0, help="Room group cell width in world units; zero disables cells")
    parser.add_argument("--margin-pixels", type=float, default=64, help="Native screen-pixel margin for static room windows")
    parser.add_argument("--host", type=Path, help="Replay using a -PageTrace host executable")
    parser.add_argument("--route", action="store_true", help="Report each room visit; requires bank-aware trace")
    parser.add_argument("--replay", type=Path, help="Unmodified HKIR input tape passed to the host")
    parser.add_argument("--data", type=Path, help="Packaged scene metadata passed to the host")
    parser.add_argument("--assets", type=Path, default=ROOT / "_private/converted-psp")
    parser.add_argument("--source", type=Path, default=Path("C:/Projects/HK3DS"))
    parser.add_argument("--frames", type=int, default=600)
    parser.add_argument("--lookahead", type=int, default=30)
    parser.add_argument("--perf", type=Path)
    parser.add_argument("--cpu", type=Path)
    parser.add_argument("--memory", type=Path)
    parser.add_argument("--output", type=Path, default=ROOT / "build/psp/page-report")
    args = parser.parse_args()
    if args.candidate_manifest is not None and not args.serialized_layout:
        parser.error("--candidate-manifest requires --serialized-layout")
    if args.serialized_layout:
        if args.host or args.bank_manifest is None or args.candidate_manifest is None or args.cell_size:
            parser.error("--serialized-layout needs --bank-manifest, --candidate-manifest and an existing trace; no cells")
        import sys
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import route_mixed_page_report
        report = route_mixed_page_report.analyze_serialized(
            args.trace, args.bank_manifest, args.candidate_manifest, args.margin_pixels)
        args.output.mkdir(parents=True, exist_ok=True)
        (args.output/'serialized-page-layout.json').write_text(json.dumps(report, indent=2)+'\n')
        print(f"{report['frames']} frames / {len(report['rooms'])} rooms; "
              f"max pinned + grown window {max(r['after']['pinned_plus_static_window'] for r in report['rooms'])} pages")
        return
    if args.route_mixed_layout:
        if args.host or args.bank_manifest is None or args.cell_size:
            parser.error("--route-mixed-layout needs --bank-manifest JSON and an existing route trace; no cells")
        import sys
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import route_mixed_page_report
        report = route_mixed_page_report.analyze(args.trace, args.bank_manifest, args.margin_pixels)
        args.output.mkdir(parents=True, exist_ok=True)
        (args.output/'route-mixed-layout.json').write_text(json.dumps(report, indent=2)+'\n')
        print(f"{report['frames']} frames / {len(report['rooms'])} rooms; "
              f"max pinned + grown window {max(r['after']['pinned_plus_static_window'] for r in report['rooms'])} pages")
        return
    if args.mixed_layout:
        if args.host or args.bank_manifest is None:
            parser.error("--mixed-layout needs --bank-manifest and an existing sprite/camera trace")
        import sys
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import mixed_page_report
        report = mixed_page_report.analyze(args.trace, args.bank_manifest, args.cell_size, args.margin_pixels)
        args.output.mkdir(parents=True, exist_ok=True)
        (args.output/'mixed-layout.json').write_text(json.dumps(report, indent=2)+'\n')
        print(f"{report['frames']} frames; pinned + static grown window "
              f"{report['after']['pinned_plus_static_window']} pages")
        return
    if args.fixed_layout:
        if args.host or args.bank_manifest is None:
            parser.error("--fixed-layout needs --bank-manifest and an existing sprite/camera trace")
        import sys
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import fixed_page_report
        report = fixed_page_report.analyze(args.trace, args.bank_manifest, args.cell_size, args.margin_pixels)
        args.output.mkdir(parents=True, exist_ok=True)
        (args.output/'fixed-layout.json').write_text(json.dumps(report, indent=2)+'\n')
        print(f"{report['frames']} frames; pinned + static grown window "
              f"{report['after']['pinned_plus_static_window']} pages; fixed-coordinate lower bound "
              f"{report['fixed_coordinate_lower_bound']['total']}")
        return
    if args.sprite_bytes:
        if args.host:
            parser.error("--sprite-bytes consumes an existing rectangle trace")
        report = analyze_sprite_bytes(args.trace)
        write_sprite_byte_report(report, args.output)
        print(f"{report['frames']} frames / {len(report['rooms'])} rooms; sprite byte lower bounds written")
        return
    if args.route_palettes:
        if args.host:
            parser.error("--route-palettes consumes an existing multi-room trace")
        report = analyze_route_palettes(args.trace)
        write_route_palette_report(report, args.output)
        print(f"{report['frames']} frames / {len(report['rooms'])} rooms; palette lower bounds written")
        return
    if args.lookahead < 0 or args.frames < 1:
        parser.error("Frames must be positive and lookahead nonnegative")
    if args.host:
        args.trace.mkdir(parents=True, exist_ok=True)
        command = [str(args.host.resolve()), "--assets", str(args.assets.resolve()),
                        "--source", str(args.source.resolve()), "--output", str(args.trace.resolve()),
                        "--frames", str(args.frames), "--every", str(args.frames), "--fast"]
        if args.replay:
            command += ["--replay", str(args.replay.resolve())]
        if args.data:
            command += ["--data", str(args.data.resolve())]
        subprocess.run(command, check=True)
    if args.route:
        report = route_demand(args.trace)
        if args.replay:
            report["inputs"][str(args.replay)] = hashlib.sha256(args.replay.read_bytes()).hexdigest()
        if args.host:
            report["inputs"][str(args.host)] = hashlib.sha256(args.host.read_bytes()).hexdigest()
        write_route_demand(report, args.output)
        print(f"{report['frames']} frames, {len(report['visits'])} visits, "
              f"max {max(v['max_simultaneous_pages'] for v in report['visits'])} simultaneous pages")
        return
    report = analyze(args.trace, args.perf, args.cpu, args.memory, args.lookahead)
    write_report(report, args.output)
    print(f"{report['frames']} frames, {len(report['stalls'])} stalls, pinned {report['pinned_bytes']} B, "
          f"resident {report['proposed_resident_bytes']} B, estimated heap {report['estimated_heap_bytes']} B")
    if report["hardware_quad_mismatch_frames"] or report["hardware_stall_frame_symmetric_difference"]:
        raise SystemExit("Replay/hardware cross-check failed; see report.json")


if __name__ == "__main__":
    main()
