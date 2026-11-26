"""Tutorial HAP1 v2 motes: retain source rates/pool, extract its shared sprite.

Run under tools/validation-lock.ps1 -Write. HK3DS inputs are read-only.
"""
import argparse
import json
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import transcode_room as room


def convert(source, output):
    package = source / "_private/converted/decor-effects"
    config = source / "_private/converted/hero/romfs/ambient-particles.bin"
    data = config.read_bytes()
    if len(data) < 24 or struct.unpack_from("<4s5I", data)[:3] != (b"HAP1", 2, 4):
        raise ValueError("Expected Tutorial HAP1 v2 / four emitters")
    _, _, emitters, capacity, sprite, edges = struct.unpack_from("<4s5I", data)
    if len(data) != 24+emitters*556+edges*20 or edges != 320 or capacity != 184:
        raise ValueError("Invalid Tutorial HAP1 size/pool")
    rates = [struct.unpack_from("<2f", data, 24+i*556+16+13*4) for i in range(emitters)]
    if rates != [(100., 100.), (50., 50.), (100., 100.), (50., 50.)]:
        raise ValueError("Tutorial source rates changed; never halve them")
    raw = (package / "decor-fx-visual.bin").read_bytes()
    if len(raw) < 20 or raw[:4] != b"HKR2":
        raise ValueError("Expected source decor HKR2")
    pages, count, draws, grass = struct.unpack_from("<4I", raw, 4)
    if len(raw) != 20+36*count+72*draws+24*grass or sprite >= count:
        raise ValueError("Invalid decor table size/sprite")
    frame = struct.unpack_from("<5I4f", raw, 20+sprite*36)
    if frame[0] >= pages or not frame[3] or not frame[4]:
        raise ValueError("Invalid mote sprite rectangle")
    Image = room.import_pil()
    _, page_count, stats, sprites = room.build_pages(Image, package, "decor-fx-page-{n}.png",
        pages, [frame], [(1., 1.)], output)
    # Retain source resolution, never enlarge it. Nearest large motes may be
    # source limited; smaller/deeper particles keep the same source density.
    clip = struct.pack("<48sf4I", b"Motes", 1., 0, 0, 0, 1)
    table = b"PSA1"+struct.pack("<6I", 1, 512, page_count, 1, 1, 1)+sprites+clip+struct.pack("<I", 0)
    (output / "motes.psp.bin").write_bytes(table)
    manifest = dict(source_sprite=sprite, source_size=list(frame[3:5]), rates=rates, capacity=capacity,
        texture_bytes=page_count*(512*512+1040), source_config_sha256=room.sha256(config),
        source_table_sha256=room.sha256(package / "decor-fx-visual.bin"), page_stats=stats,
        remaining="Dynamic behind-plane motes are not composite-blurred; source edge-emission approximations are inherited.")
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2)+"\n")
    return manifest


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=room.source_root())
    parser.add_argument("--out", type=Path, default=room.REPO / "_private/converted-psp/motes")
    args = parser.parse_args()
    print(json.dumps(convert(args.source, args.out), indent=2))
