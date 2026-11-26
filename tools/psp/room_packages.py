"""Convert selected route legs using read-only HK3DS inputs.

Accepts a Stage A JSON or the HK3DS ordered itinerary census. No input tape is changed.
"""
import argparse
from concurrent.futures import ProcessPoolExecutor, as_completed
import hashlib
import json
import os
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import transcode_room as room
import regen_stamp

# 2026-11-20 (user: "speed up regen, look at the 3DS"): HK3DS tools/regen.py runs converters in parallel workers
# and skips rules whose inputs are unchanged. routerooms converted ~120 rooms one by one every time (~1 min each).
# Each room now records an input key (converter code hash + name/size/mtime of every input file); an unchanged
# room is skipped, and changed rooms convert in parallel processes.
KEY_FILE = ".input-key"
WORKERS = max(1, min(6, (os.cpu_count() or 2) // 2))

SIDECARS = {"plants": "static-plants.bin", "dynamics": "static-room-dynamics.bin",
            "ambient": "static-room-ambient.bin", "grass": "static-grass.bin"}
DEFAULT_LEGS = (1, 2)
DEFAULT_STAGE = room.REPO / "tests/stages/stage-a.json"
DREAM_SCENES = {"Dream_Nailcollection": 394, "Dream_Guardian_Lurien": 401,
                "Dream_Guardian_Hegemol": 400}


def route_steps(stage: dict, legs=DEFAULT_LEGS) -> list[dict]:
    steps = stage["steps"]
    missing = set(legs) - {step["leg"] for step in steps}
    if missing:
        raise ValueError(f"Missing route legs: {sorted(missing)}")
    selected = [step for step in steps if step["leg"] in legs]
    if not selected:
        raise ValueError("No route steps selected")
    for step in selected:
        for scene in (step["from"], step["to"]):
            if not isinstance(scene, str) or not re.fullmatch(r"[A-Za-z0-9_-]+", scene):
                raise ValueError(f"Invalid scene identifier: {scene}")
    return selected


def route_rooms(stage: Path, legs=DEFAULT_LEGS) -> list[str]:
    """Order by first appearance in the accepted tape, including detours."""
    steps = route_steps(json.loads(stage.read_text(encoding="utf-8")), legs)
    return list(dict.fromkeys(scene for step in steps for scene in (step["from"], step["to"])))


def scene_directory(root: Path, scene: str, assets: Path | None) -> Path:
    if scene in DREAM_SCENES and assets is not None:
        return assets / "dream-host/romfs/scenes" / scene
    return root / scene


def census_itinerary(census: Path):
    text = census.read_text(encoding="utf-8-sig")
    heading = re.search(r"(?m)^## Ordered itinerary\s*$", text)
    if not heading:
        raise ValueError(f"Missing ordered itinerary: {census}")
    table = re.split(r"(?m)^## ", text[heading.end():], maxsplit=1)[0]
    chains = {}
    for number, chain in re.findall(r"(?m)^\| (\d+) \| ([^|]+) \|", table):
        leg = int(number)
        scenes = re.findall(r"`([A-Za-z0-9_-]+)`\[([A-Z])(?:/[A-Z])?\]", chain)
        if leg in chains or len(scenes) != chain.count("\u2192") + 1:
            raise ValueError(f"Invalid ordered itinerary chain for leg {leg}")
        chains[leg] = scenes
    if not chains or set(chains) != set(range(1, max(chains) + 1)):
        raise ValueError(f"Empty or nonconsecutive ordered itinerary legs: {census}")
    return chains


def census_rooms(source: Path, census: Path, legs=None, assets=None) -> tuple[list[str], list[str], list[int]]:
    chains = census_itinerary(census)
    selected = sorted(chains if legs is None else set(legs))
    if not selected or set(selected) - chains.keys():
        raise ValueError(f"Missing ordered itinerary legs: {sorted(set(selected) - chains.keys())}")
    root = source / "_private/converted/hero/romfs/scenes"
    ready, excluded = {}, {}
    for leg in selected:
        for scene, status in chains[leg]:
            simulation = scene_directory(root, scene, assets) / "scene.bin"
            if scene == "Tutorial_01" or simulation.is_file():
                ready[scene] = None
            elif status == "U" and scene not in DREAM_SCENES and not simulation.parent.exists():
                excluded[scene] = None
            else:
                if scene in DREAM_SCENES:
                    raise FileNotFoundError(f"{simulation}; run regen-assets.ps1 -Only dreamscenes")
                raise FileNotFoundError(simulation)
    return list(ready), list(excluded), selected


def selected_rooms(source: Path, selection: Path, legs=None, assets=None):
    if selection.suffix.casefold() == ".md":
        return census_rooms(source, selection, legs, assets)
    legs = DEFAULT_LEGS if legs is None else legs
    return route_rooms(selection, legs), [], sorted(set(legs))


def explicit_rooms(source: Path, scenes, assets=None):
    root = source / "_private/converted/hero/romfs/scenes"
    unique = {}
    for scene in scenes:
        if not re.fullmatch(r"[A-Za-z0-9_-]+", scene):
            raise ValueError(f"Invalid room name: {scene!r}")
        simulation = scene_directory(root, scene, assets) / "scene.bin"
        if not simulation.is_file():
            raise FileNotFoundError(simulation)
        unique[scene] = None
    return list(unique)


def source_package(source: Path, scene: str, assets=None) -> tuple[Path, str, str, Path, dict]:
    converted = source / "_private/converted"
    root = converted / "hero/romfs/scenes"
    local = scene_directory(root, scene, assets)
    if local != root / scene:
        return (assets / "dream-host/scenes" / scene / "art", "room-visual.bin",
                "room-page-{n}.png", local, {})
    if scene == "Tutorial_01":
        return (converted / "tutorial-static", "static-room-visual.bin", "static-room-page-{n}.png",
                converted / "tutorial-static", dict(SIDECARS))
    # Generic host rooms only require a baked sidecar when its original exists.
    original = converted / "hero/romfs/scenes" / scene
    sidecars = {kind: name.removeprefix("static-") for kind, name in SIDECARS.items()
                if (original / name.removeprefix("static-")).is_file()}
    art = converted / "static-decor" / scene
    baked = converted / "hero/romfs/static-scenes" / scene
    if (art / "static-room-visual.bin").is_file() and all(
            (baked / ("static-" + name)).is_file() for name in sidecars.values()):
        return (art, "static-room-visual.bin", "static-room-page-{n}.png", baked,
                {kind: "static-" + name for kind, name in sidecars.items()})
    return (converted / "scenes" / scene / "art", "room-visual.bin",
            "room-page-{n}.png", original, sidecars)


def input_key(source: Path, output: Path, scene: str, code: str) -> str:
    art, visual, pattern, sidecar_root, sidecars = source_package(source, scene, output)
    research = source / "_private/research/crossroads-scenes" / scene
    files = [p for folder in (art, sidecar_root) if folder.is_dir() for p in folder.iterdir() if p.is_file()]
    if research.is_dir():
        files += [p for p in research.rglob("*") if p.is_file()]
    if scene == room.archive_particles.SCENE:
        files += room.archive_particles.inputs(source)
    if scene == room.MATO_SCENE:
        files += room.mato_inputs(source)
    digest = hashlib.sha256("\n".join((code, visual, pattern, repr(sorted(sidecars.items())), "")).encode())
    for path in sorted(set(files)):
        stat = path.stat()
        digest.update("\t".join((path.as_posix(), str(stat.st_size), str(stat.st_mtime_ns))).encode() + b"\n")
    return digest.hexdigest()


def cached_manifest(destination: Path, key: str):
    try:
        if (destination / KEY_FILE).read_text(encoding="utf-8").strip() != key:
            return None
        manifest = json.loads((destination / "manifest.json").read_text(encoding="utf-8"))
        result = {name: manifest[name] for name in ("pages", "texture_bytes", "sprites", "draws")}
    except (OSError, ValueError, KeyError):
        return None
    if not all((destination / f"page-{page}.hpt").is_file() for page in range(result["pages"])):
        return None
    if destination.name == room.archive_particles.SCENE and not (destination / room.archive_particles.BANK).is_file():
        return None
    return result


def convert_room(source: Path, output: Path, scene: str, key: str):
    art, visual, pattern, sidecar_root, sidecars = source_package(source, scene, output)
    destination = output / scene
    destination.mkdir(parents=True, exist_ok=True)
    (destination / KEY_FILE).unlink(missing_ok=True)
    manifest = room.transcode(art, visual, pattern, destination, scene, sidecars,
                              sidecar_package=sidecar_root,
                              research=source / "_private/research/crossroads-scenes")
    (destination / KEY_FILE).write_text(key + "\n", encoding="utf-8")
    return scene, {"pages": manifest["pages"], "texture_bytes": manifest["texture_bytes"],
                   "sprites": manifest["sprites"], "draws": manifest["draws"]}


def convert(source: Path, output: Path, stage: Path, legs=None, rooms=None) -> dict:
    if rooms is None:
        scenes, excluded, selected_legs = selected_rooms(source, stage, legs, output)
    else:
        scenes, excluded, selected_legs = explicit_rooms(source, rooms, output), [], []
    manifests = {}
    # Validate every input before spending time on any conversion.
    for scene in scenes:
        art, visual, pattern, sidecar_root, sidecars = source_package(source, scene, output)
        table = room.RoomTable((art / visual).read_bytes())
        for name in sidecars.values():
            if not (sidecar_root / name).is_file():
                raise FileNotFoundError(sidecar_root / name)
        for page in range(table.pages):
            if not (art / pattern.format(n=page)).is_file():
                raise FileNotFoundError(art / pattern.format(n=page))
    code = regen_stamp.code_hash(Path(__file__))
    todo = []
    for scene in scenes:
        key = input_key(source, output, scene, code)
        cached = cached_manifest(output / scene, key)
        if cached is None:
            todo.append((scene, key))
        else:
            manifests[scene] = cached
            print(f"{scene}: unchanged, {cached['pages']} pages", flush=True)
    if todo:
        with ProcessPoolExecutor(max_workers=min(WORKERS, len(todo))) as pool:
            jobs = {pool.submit(convert_room, source, output, scene, key): scene for scene, key in todo}
            for job in as_completed(jobs):
                scene, manifest = job.result()
                manifests[scene] = manifest
                print(f"{scene}: {manifest['pages']} pages, {manifest['texture_bytes']} texture bytes", flush=True)
    manifests = {scene: manifests[scene] for scene in scenes}
    result = {"schema": 1, "legs": selected_legs, "rooms": manifests}
    if rooms is None:
        result["stage_sha256"] = room.sha256(stage)
    if excluded:
        result["excluded"] = excluded
    index = "rooms.json" if rooms is None else "explicit-rooms.json"
    (output / index).write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=room.source_root())
    parser.add_argument("--out", type=Path, default=room.REPO / "_private/converted-psp")
    parser.add_argument("--stage", type=Path, default=DEFAULT_STAGE,
                        help="Stage JSON or HK3DS minimum-route.md (all census legs by default)")
    parser.add_argument("--legs", nargs="+", type=int)
    parser.add_argument("--rooms", nargs="+", help="Convert only the named rooms instead of route legs")
    args = parser.parse_args()
    if args.rooms and args.legs:
        parser.error("--rooms cannot be combined with --legs")
    result = convert(args.source, args.out, args.stage, args.legs, args.rooms)
    if result.get("excluded"):
        print("No HK3DS scene directory: " + ", ".join(result["excluded"]))
    print(f"Converted {len(result['rooms'])} room packages for legs {result['legs']}")


if __name__ == "__main__":
    main()
