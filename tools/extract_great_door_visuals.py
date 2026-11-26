"""Export the Tutorial_01 Great Door tk2d collection from AssetRipper."""
import argparse
import hashlib
import json
from pathlib import Path

from assetripper_client import Client, ROOT
from extract_crawler import Export

BUILD_ID = 22529139
SCENE_EXPORTS = {
    "sprite": "_private/research/great-door/level6-11923.json",
    "object": "_private/research/great-door/level6-1806.json",
    "transform": "_private/research/room-visuals/level6-5674.json",
    "parent_transform": "_private/research/room-visuals/level6-5677.json",
    "root_transform": "_private/research/room-visuals/level6-6759.json",
}
SPRITE_NAMES = (
    "door_v01", "door_v02", "door_v03",
    "additional_destruction_0002_a", "additional_destruction_0001_a",
    "additional_destruction_0000_a",
)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def extract(client, output):
    output = output.absolute()
    if not output.is_relative_to(ROOT / "_private"):
        raise ValueError("Original resources must remain private")
    output.mkdir(parents=True, exist_ok=True)

    rows = [row for row in client.search("Tute Great Door")
            if row["type"] == "GameObject" and row["name"] == "Tute Great Door"
            and row["collection"] == "sharedassets6.assets"]
    if len(rows) != 1:
        raise ValueError("Load the Tutorial_01 source build with sharedassets6.assets")
    row = rows[0]
    exporter = Export(client, output)
    parts = exporter.components(row["path"]["C"], row["path"]["D"])
    collections = parts.get("tk2dSpriteCollectionData", [])
    if len(collections) != 1:
        raise ValueError("Expected one Great Door tk2dSpriteCollectionData")
    collection_name = collections[0]
    collection = exporter.cache[collection_name]["m_Structure"]
    definitions = collection["spriteDefinitions"]
    sprite_ids = {definition["name"]: index for index, definition in enumerate(definitions)}
    if any(name not in sprite_ids for name in SPRITE_NAMES):
        raise ValueError("Great Door source collection is missing one of its three door sprites")
    if len(collection["textures"]) != 1:
        raise ValueError("Expected one Great Door atlas texture")
    texture_ref = collection["textures"][0]
    texture, texture_name, texture_collection = exporter.resolve(row["path"]["C"], texture_ref)
    png_name = texture_name.removesuffix(".json") + ".png"
    raw = client.request("/Assets/Image", Path=json.dumps(
        {"C": texture_collection, "D": texture_ref["m_PathID"]}), Extension="png")
    (output / png_name).write_bytes(raw)
    exporter.hashes[png_name] = hashlib.sha256(raw).hexdigest()

    scene_hashes = {}
    for role, relative in SCENE_EXPORTS.items():
        path = ROOT / relative
        if not path.is_file():
            raise ValueError("Missing researched source context: " + relative)
        scene_hashes[relative] = sha256(path)
    source_sprite = json.loads((ROOT / SCENE_EXPORTS["sprite"]).read_text(encoding="utf-8"))
    scene_collection = source_sprite["m_Structure"]["collection"]
    if scene_collection["m_PathID"] != 1125:
        raise ValueError("Saved Great Door scene sprite references a different collection")

    mapping = {
        "build_id": BUILD_ID,
        "scene": "Tutorial_01",
        "owner": row["path"]["D"],
        "owner_name": row["name"],
        "collection": collection_name,
        "collection_path_id": 1125,
        "texture": png_name,
        "texture_dimensions": [texture["m_Width"], texture["m_Height"]],
        "sprite_ids": {name: sprite_ids[name] for name in SPRITE_NAMES},
        "scene_sources_sha256": scene_hashes,
        "source_sha256": exporter.hashes,
        "raster_scale": 0.5,
        "raster_filter": "premultiplied-alpha box",
    }
    (output / "great-door-visuals-map.json").write_text(
        json.dumps(mapping, indent=2), encoding="utf-8")
    print(json.dumps({"sprites": mapping["sprite_ids"], "texture": mapping["texture_dimensions"],
                      "source_files": len(exporter.hashes)}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "_private/research/great-door-visuals")
    parser.add_argument("--port", type=int, default=58291)
    args = parser.parse_args()
    extract(Client(args.port), args.output)
