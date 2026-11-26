"""Font ladder for the PSP UI (port-plan 5.1): the two original faces at PSP pixel sizes.

Faces (original game files, extracted by the 3DS converters, read-only):
  TrajanPro-Bold  _private/research/title-menu/font.otf   menus, titles, area names, Geo digits
  Perpetua        _private/research/fury-chest/Perpetua.ttf  dialogue, lore, prompts, descriptions

For each face and pixel size the tool measures the capital height (ink rows of 'H'), the mean
advance and the characters per 440 px line, and renders 480x272 ladder frames twice: 8-bit
antialiased and quantised to 16 grey levels (the CLUT4 atlas the plan proposes). View the
frames 1:1 on a PSP screen or in PPSSPP at 1x before choosing sizes.

Output: build/psp/fonts/ladder-<face>-<n>.png, build/psp/fonts/ladder-<face>-<n>-clut4.png,
        build/psp/fonts/fonts.json (metrics and the smallest size meeting each floor).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from transcode_room import import_pil, source_root  # noqa: E402

FACES = {
    "trajan": "_private/research/title-menu/font.otf",
    "perpetua": "_private/research/fury-chest/Perpetua.ttf",
}
FLOORS = {"body_caps_px": 9, "heading_caps_px": 11}  # port-plan 5.1, user decision 3
SAMPLE = "The quick brown fox jumps over the lazy dog. 0123456789"
CAPS = "HALLOWNEST KINGDOM GEO 1234"
LINE_PX = 440


def cap_height(ImageFont, font) -> int:
    box = font.getbbox("H")
    return box[3] - box[1]


def measure(ImageFont, path: Path, sizes):
    rows = []
    for size in sizes:
        font = ImageFont.truetype(str(path), size)
        advance = font.getlength(SAMPLE) / len(SAMPLE)
        rows.append({"size_px": size, "cap_px": cap_height(ImageFont, font),
                     "mean_advance_px": round(advance, 2), "chars_per_line": int(LINE_PX // advance)})
    return rows


def frames(Image, ImageDraw, ImageFont, path: Path, rows, name: str, out: Path):
    written, frame, y, index = [], None, 0, 0

    def flush():
        nonlocal frame, index
        if frame is None:
            return
        frame.save(out / f"ladder-{name}-{index}.png")
        frame.point(lambda v: (v * 15 + 127) // 255 * 17).save(out / f"ladder-{name}-{index}-clut4.png")
        written.append(f"ladder-{name}-{index}.png")
        frame, index = None, index + 1

    for row in rows:
        font = ImageFont.truetype(str(path), row["size_px"])
        label = ImageFont.truetype(str(path), 12)
        height = row["size_px"] * 2 + 18
        if frame is None or y + height > 272:
            flush()
            frame, y = Image.new("L", (480, 272), 0), 4
        draw = ImageDraw.Draw(frame)
        draw.text((4, y), f"{row['size_px']}px  caps {row['cap_px']}px  {row['chars_per_line']}/line", font=label, fill=128)
        draw.text((20, y + 14), CAPS if name == "trajan" else SAMPLE, font=font, fill=255)
        draw.text((20, y + 14 + row["size_px"]), SAMPLE.upper() if name == "trajan" else CAPS.title(), font=font, fill=220)
        y += height
    flush()
    return written


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, default=REPO / "build/psp/fonts")
    parser.add_argument("--sizes", default="10-22")
    args = parser.parse_args(argv)
    lo, hi = (int(x) for x in args.sizes.split("-"))
    Image = import_pil()
    from PIL import ImageDraw, ImageFont
    args.out.mkdir(parents=True, exist_ok=True)
    report = {"floors": FLOORS, "line_px": LINE_PX, "faces": {}}
    for name, rel in FACES.items():
        path = source_root() / rel
        rows = measure(ImageFont, path, range(lo, hi + 1))
        report["faces"][name] = {
            "source": rel, "sha256": hashlib.sha256(path.read_bytes()).hexdigest(), "sizes": rows,
            "smallest_body_px": next((r["size_px"] for r in rows if r["cap_px"] >= FLOORS["body_caps_px"]), None),
            "smallest_heading_px": next((r["size_px"] for r in rows if r["cap_px"] >= FLOORS["heading_caps_px"]), None),
            "frames": frames(Image, ImageDraw, ImageFont, path, rows, name, args.out),
        }
    (args.out / "fonts.json").write_text(json.dumps(report, indent=1), encoding="utf-8")
    for name, face in report["faces"].items():
        print(f"{name}: body floor at {face['smallest_body_px']} px, heading floor at {face['smallest_heading_px']} px; "
              + ", ".join(f"{r['size_px']}:{r['cap_px']}/{r['chars_per_line']}" for r in face["sizes"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
