"""Build the Great Door HKA1 sprite bank and its bounded texture page."""
import converted_files  # Install hardlink copy-on-write before asset writes.
import argparse
import hashlib
import json
import math
import shutil
import struct
import tex3ds_cache
from pathlib import Path

from PIL import Image

from assetripper_client import ROOT
from convert_hero import decode_sprite
from tool_paths import TEX3DS as TEX3DS_EXE

NAMES = (
    "door_v01", "door_v02", "door_v03",
    "additional_destruction_0002_a", "additional_destruction_0001_a",
    "additional_destruction_0000_a",
)
TRANSFORM_FILES = (
    "_private/research/room-visuals/level6-5674.json",
    "_private/research/room-visuals/level6-5677.json",
    "_private/research/room-visuals/level6-6759.json",
)
PAGE_WIDTH, PAGE_HEIGHT = 1024, 512
RASTER_SCALE = 0.5


def checked_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def world_transform(transforms):
    position = [0.0, 0.0, 0.0]
    scale = [1.0, 1.0, 1.0]
    for transform in reversed(transforms):
        rotation = transform["m_LocalRotation"]
        if rotation != {"m_W": 1, "m_X": 0, "m_Y": 0, "m_Z": 0}:
            raise ValueError("Rotated Great Door transform needs explicit support")
        local = transform["m_LocalPosition"]
        local_scale = transform["m_LocalScale"]
        for index, axis in enumerate(("m_X", "m_Y", "m_Z")):
            position[index] += local[axis] * scale[index]
            scale[index] *= local_scale[axis]
    if not all(math.isfinite(value) for value in position + scale) or min(scale) == 0:
        raise ValueError("Invalid Great Door world transform")
    return position, scale


def downsample(sprite):
    size = (math.ceil(sprite.width * RASTER_SCALE), math.ceil(sprite.height * RASTER_SCALE))
    return sprite.convert("RGBa").resize(size, Image.Resampling.BOX).convert("RGBA")


def convert(source, output, romfs, tex3ds):
    output = output.absolute()
    romfs = romfs.absolute()
    if not converted_files.private_output(output, ROOT, allow_build=True):
        raise ValueError("Output must stay inside _private or build")
    if not romfs.is_relative_to(ROOT / "_private"):
        raise ValueError("Converted original resources must remain private")
    output.mkdir(parents=True, exist_ok=True)
    romfs.mkdir(parents=True, exist_ok=True)
    mapping = checked_json(source / "great-door-visuals-map.json")
    if mapping["build_id"] != 22529139 or mapping["sprite_ids"] != {name: i for i, name in enumerate(NAMES)}:
        raise ValueError("Unexpected Great Door source build or sprite map")
    for name, expected in mapping["source_sha256"].items():
        if sha256(source / name) != expected:
            raise ValueError("Changed source export: " + name)
    for relative, expected in mapping["scene_sources_sha256"].items():
        if sha256(ROOT / relative) != expected:
            raise ValueError("Changed scene transform evidence: " + relative)

    scene_sprite = checked_json(ROOT / "_private/research/great-door/level6-11923.json")["m_Structure"]
    if scene_sprite["_spriteId"] != 0 or scene_sprite["collection"]["m_PathID"] != mapping["collection_path_id"]:
        raise ValueError("Great Door default sprite no longer matches the original collection")
    if scene_sprite["_scale"] != {"x": 1, "y": 1, "z": 1}:
        raise ValueError("Scaled Great Door sprite needs separate transform support")
    transform_files = [checked_json(ROOT / path) for path in TRANSFORM_FILES]
    position, world_scale = world_transform(transform_files)
    collection = checked_json(source / mapping["collection"])["m_Structure"]
    atlas = Image.open(source / mapping["texture"]).convert("RGBA")
    sprite_records = []
    sprites = []
    page = Image.new("RGBA", (PAGE_WIDTH, PAGE_HEIGHT), (0, 0, 0, 0))
    x, y = 1, 1
    for name in NAMES:
        sprite_id = mapping["sprite_ids"][name]
        definition = collection["spriteDefinitions"][sprite_id]
        if definition["name"] != name:
            raise ValueError("Sprite ID/name mismatch for " + name)
        bitmap, local_bounds = decode_sprite(atlas, definition)
        original_size = bitmap.size
        bitmap = downsample(bitmap)
        if x + bitmap.width + 1 > PAGE_WIDTH or y + bitmap.height + 1 > PAGE_HEIGHT:
            raise ValueError("Great Door artwork exceeds 1024x512 page")
        page.paste(bitmap, (x, y))
        left, bottom, right, top = local_bounds
        bounds = (
            position[0] + left * world_scale[0],
            position[1] + bottom * world_scale[1],
            position[0] + right * world_scale[0],
            position[1] + top * world_scale[1],
        )
        if not all(math.isfinite(value) for value in bounds) or bounds[2] <= bounds[0] or bounds[3] <= bounds[1]:
            raise ValueError("Invalid world bounds for " + name)
        sprite_records.append((0, x, y, bitmap.width, bitmap.height, *bounds))
        sprite_meta = dict(name=name, sprite_id=sprite_id, source_pixels=list(original_size),
                           packed_pixels=list(bitmap.size), local_bounds=list(local_bounds),
                           world_bounds=list(bounds), atlas_xy=[x, y])
        sprite_meta["source_sha256"] = mapping["source_sha256"][mapping["texture"]]
        sprites.append(sprite_meta)
        x += bitmap.width + 2

    clips = []
    for index, name in enumerate(NAMES):
        clips.append((name, 1.0, 2, 0, index, 1))
    with (output / "great-door-visual.bin").open("wb") as bank:
        bank.write(struct.pack("<4s4I", b"HKA1", 1, len(sprite_records), len(clips), len(clips)))
        for record in sprite_records:
            bank.write(struct.pack("<5I4f", *record))
        for clip in clips:
            encoded = clip[0].encode("ascii")
            bank.write(struct.pack("<48sf4I", encoded, *clip[1:]))
        for index in range(len(clips)):
            bank.write(struct.pack("<I", index))

    png = output / "great-door-page-0.png"
    t3x = output / "great-door-page-0.t3x"
    page.save(png)
    tex3ds_cache.run([str(tex3ds), "-f", "rgba8", "-z", "none", "-o", str(t3x), str(png)], check=True)
    for name in ("great-door-visual.bin", "great-door-page-0.t3x"):
        shutil.copyfile(output / name, romfs / name)
    report = {
        "build_id": mapping["build_id"],
        "source_sha256": mapping["source_sha256"],
        "scene_sources_sha256": mapping["scene_sources_sha256"],
        "format": "HKA1",
        "page_size": [PAGE_WIDTH, PAGE_HEIGHT],
        "texture_format": "rgba8",
        "texture_bytes": PAGE_WIDTH * PAGE_HEIGHT * 4,
        "raster_scale": RASTER_SCALE,
        "source_position": position,
        "source_scale": world_scale,
        "depth": position[2],
        "sprites": sprites,
        "clips": [dict(name=name, fps=1.0, wrapMode=2, loopStart=0, frames=1) for name in NAMES],
        "atlas_occupancy_pixels": sum(s["packed_pixels"][0] * s["packed_pixels"][1] for s in sprites),
    }
    (output / "manifest.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({"sprites": len(sprites), "depth": position[2], "page_bytes": report["texture_bytes"],
                      "packed": [s["packed_pixels"] for s in sprites]}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "_private/research/great-door-visuals")
    parser.add_argument("--output", type=Path, default=ROOT / "_private/converted/great-door-visuals")
    parser.add_argument("--romfs", type=Path, default=ROOT / "_private/converted/hero/romfs")
    parser.add_argument("--tex3ds", type=Path, default=Path(TEX3DS_EXE))
    args = parser.parse_args()
    convert(args.source, args.output, args.romfs, args.tex3ds)
