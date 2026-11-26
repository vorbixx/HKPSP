"""HKL4 geometry stays unchanged; PSP-only PSL1 rectangles name HPT1 light pages.

Run under tools/validation-lock.ps1 -Write. Inputs are read-only HK3DS PNGs.
Alpha is folded into neutral .5. Both backends decode HeroLight's positive
signed additive operand; the donut uses capped multiply plus an intercept.
"""
import argparse
import json
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import transcode_room as room


def operands(image, kind, alpha=1):
    Image = room.import_pil()
    pixels = []
    for *rgb, a in image.getdata():
        a = a / 255 * alpha
        if kind == "linear":
            values = [.5 * (1-a) + c/255*a for c in rgb]
        else:
            # HK3DS PNG stores VividLight upper-branch gain / 4.
            gain = [4*c/255 for c in rgb]
            if kind == "multiply":
                values = [(1-a + a*min(2, g))/2 for g in gain]
            else:
                # Approximation: residual intercept preserves the gain's white
                # threshold at full alpha; the plan permits flatter highlights.
                values = [a*max(0, 1-2/g) if g else 0 for g in gain]
        pixels.append(tuple(round(max(0, min(1, c))*255) for c in values)+(255,))
    result = Image.new("RGBA", image.size)
    result.putdata(pixels)
    return result


def validate_light_gradient(source, decoded):
    """Reject a palette that introduces a rim into the folded light operand.

    One folded-byte error becomes two signed-additive bytes. Two neighbouring
    taps can therefore differ from the source step by at most four bytes.
    Neutral pixels must stay exact: their error would light the quad's edge.
    This is a source-texture contract, not an original-scene look threshold.
    """
    if source.size != decoded.size:
        raise ValueError("HeroLight gradient dimensions disagree")
    for expected, actual in zip(source.getdata(), decoded.getdata()):
        if actual[3] != 255 or (expected[:3] == (128, 128, 128) and actual != expected):
            raise ValueError("HeroLight palette changed an opaque neutral texel")
        if any(abs(a-b) > 1 for a, b in zip(expected[:3], actual[:3])):
            raise ValueError("HeroLight palette lost the source gradient")


def convert(source, out):
    Image = room.import_pil()
    raw = (source / "lighting.bin").read_bytes()
    if len(raw) != 364 or raw[:4] != b"HKL4":
        raise ValueError("Expected exact HKL4 lighting table")
    sprites = [struct.unpack_from("<2I4f", raw, 4+i*24) for i in range(3)]
    transform = struct.unpack_from("<6f", raw, 76)
    scales = struct.unpack_from("<6f", raw, 100)
    glow = struct.unpack_from("<6f", raw, 320)
    names = ["hero-light", "vignette", "hero-glow"]
    images, factors, records = [], [], []
    for i, (name, sprite) in enumerate(zip(names, sprites)):
        w, h, left, bottom, right, top = sprite
        image = Image.open(source / (name+".png")).convert("RGBA")
        # 3DS converter pads at the fixed top-left origin to powers of two.
        if image.width < w or image.height < h or max(image.size) > 512:
            raise ValueError("HKL4/PNG dimensions disagree: "+name)
        image = image.crop((0, 0, w, h))
        sx, sy, depth = ((transform[0], transform[0], transform[2]) if i == 0 else
                         (max(scales), max(scales), transform[5]) if i == 1 else
                         (glow[0], glow[1], glow[4]))
        ppu = room.room_focal_length(480, 272)/(38.1+depth)
        factor = ((right-left)*sx*ppu/w, (top-bottom)*sy*ppu/h)
        size = room.target_size(w, h, factor)
        image = room.resample(Image, image, size)
        records.append({"name": name, "source_size": [w, h], "size": size,
                        "screen_pixels_per_source_texel": factor,
                        "source_limited": max(factor) > 1})
        images.append(image)
        factors.append(sprite[2:])
    prepared = [operands(images[0], "linear", transform[3]), images[1],
                operands(images[2], "multiply", glow[5]), operands(images[2], "add", glow[5]),
                Image.new("RGBA", (1, 1), (0, 0, 0, 255))]
    geometry = [factors[0], factors[1], factors[2], factors[2], (0, 0, 1, 1)]
    places, count = room.pack([im.size for im in prepared])
    pages = [Image.new("RGBA", (512, 512)) for _ in range(count)]
    table = bytearray(b"PSL1"+struct.pack("<4I", 1, 512, count, 5))
    for image, bounds, (page, x, y) in zip(prepared, geometry, places):
        room.paste_padded(pages[page], image, x, y)
        table += struct.pack("<5I4f", page, x, y, *image.size, *bounds)
    out.mkdir(parents=True, exist_ok=True)
    stats = []
    for n, image in enumerate(pages):
        quant = room.quantize_page(Image, image)  # shared median-cut; the octree palette ringed the glow (psp-haze)
        page, x, y = places[0]
        if n == page:
            w, h = prepared[0].size
            validate_light_gradient(prepared[0], quant.convert("RGBA").crop((x, y, x+w, y+h)))
        palette = bytes(quant.getpalette(rawmode="RGBA"))
        palette += bytes(1024-len(palette))
        (out / f"page-{n}.hpt").write_bytes(room.hpt1(512, 512, palette, quant.tobytes()))
        quant.convert("RGBA").save(out / f"page-{n}.png")
        stats.append(room.premultiplied_psnr(image.tobytes(), quant.convert("RGBA").tobytes()))
    (out / "lighting.psp.bin").write_bytes(table)
    manifest = {"inputs": {p.name: room.sha256(p) for p in [source/"lighting.bin"]+
                           [source/(n+".png") for n in names]},
                "sprites": records, "pages": count, "page_psnr_db": stats,
                "texture_bytes": count*(512*512+1040),
                "approximation": "signed additive light with alpha folded before saturation; donut capped gain plus residual additive intercept"}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2)+"\n")
    return manifest


def convert_lantern_glow(source, out):
    Image = room.import_pil()
    clip = (source / "lantern-glow.bin").read_bytes()
    image = Image.open(source / "lantern-glow.png").convert("RGBA")
    if image.size != (512, 256) or len(clip) != 23228 or clip[:8] != b"LNG1" + struct.pack("<I", 362):
        raise ValueError("Expected the authored LNG1 clip and 512x256 glow atlas")
    quant = room.quantize_page(Image, image)
    palette = bytes(quant.getpalette(rawmode="RGBA"))
    palette += bytes(1024-len(palette))
    (out / "lantern-glow.hpt").write_bytes(room.hpt1(512, 256, palette, quant.tobytes()))
    (out / "lantern-glow.bin").write_bytes(clip)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--source", type=Path, default=room.source_root()/"_private/converted/hero-light")
    parser.add_argument("--out", type=Path, default=room.REPO/"_private/converted-psp/hero-light")
    args = parser.parse_args()
    manifest = convert(args.source, args.out)
    convert_lantern_glow(args.source.parent / "lantern-glow", args.out)
    print(json.dumps(manifest, indent=2))
