"""PSP transcoder for one converted room package (first target: Tutorial_01).

Input: the 3DS converted package (read-only, in the HK3DS tree): an HKR3/HKR4
room-visual table and its native-resolution RGBA atlas pages (PNG).
Output (_private/converted-psp/<scene>/):
  room-visual.psp.bin  PSR1 table: resized/padded sprites and blurred variants;
                       source draw transforms, grass and split are retained.
  page-N.hpt           HPT1 texture: CLUT8 (GU_PSM_T8) with an RGBA8888
                       (GU_PSM_8888) palette, swizzled 16x8-byte blocks.
  page-N.png           unswizzled 32-bit preview of the same page (checks).
  static-*.bin         sidecars with PSR1 sprite counts and ambient frame remaps.
  manifest.json        source hashes, per-sprite density factors, page PSNR.

Density rule (docs/psp/port-plan.md 2.2, 4.1): every sprite is resampled so
that one texel covers at most one PSP screen pixel at the largest scale any of
its draws uses; it is never upscaled. Resampling is an area (box) filter in
linear light on premultiplied alpha.
Behind-plane variants use four premultiplied-sRGB blur passes before downscaling;
additional repeated halving is accepted only at >= 40 dB reconstruction PSNR.

Animation frames that no draw names (plants, spikes, glow bugs) take the factor
of the draws they are swapped into (tools/psp/room_sidecars.py); HKS1 scale
curves raise the factor by their maximum. Not done here (remaining work,
docs/tasks/psp-tutorial-transcode.md): particle
sprites, colour-similarity page grouping,
static vertex buffers.
"""
from __future__ import annotations

import argparse
import gc
import hashlib
import heapq
import json
import math
import os
import re
import struct
import sys
from array import array
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))  # shared animation event parser under -I
sys.path.insert(0, str(Path(__file__).resolve().parent))  # -I drops the script folder
import archive_particles
PSP_WIDTH, PSP_HEIGHT = 480, 272
HERO_DEPTH = 38.1
PAGE = 512
PAD = 1
SPRITE_BYTES = 36
GU_PSM_8888, GU_PSM_T8 = 3, 5
PSR_VERSION = 1
BLUR_OFFSETS = (0, 1.384615, -.615385, 3.230769, -2.769231)
BLUR_WEIGHTS = (.227027, .316261, .316261, .070270, .070270)
BLUR_PSP_SPACING = .75  # 1920/640 original pixels * 480/1920
BLUR_VIBRANCE = .2


def source_root() -> Path:
    configured = os.environ.get('HKPSP_SOURCE_ROOT')
    if configured:
        return Path(configured).resolve()
    if ((REPO / '_private/original/Hollow Knight/hollow_knight_Data').is_dir()
            or (REPO / '_private/converted/hero/romfs/tutorial-room.bin').is_file()):
        return REPO
    return next((p / 'HK3DS' for p in REPO.parents[:2] if (p / 'HK3DS').is_dir()), REPO)


def import_pil():
    try:
        from PIL import Image  # noqa: F401
    except ImportError:
        path = os.environ.get("HKPSP_PIL_PATH", str(source_root() / "_private/tools/boss-python"))
        sys.path.insert(0, path)
    from PIL import Image
    return Image


def room_focal_length(width: float, height: float) -> float:
    """Same as src/room_visual.cpp roomFocalLength."""
    aspect = max(1.6, width / height)
    fov = 24.0 * max(1.0, (16.0 / 9.0) / aspect)
    return height / (2 * math.tan(fov * math.pi / 360))


# ---------------------------------------------------------------- HKR tables
class RoomTable:
    def __init__(self, data: bytes):
        magic = data[:4]
        if magic not in (b"HKR3", b"HKR4"):
            raise ValueError(f"unsupported room table {magic!r} (HKR3/HKR4 only)")
        self.magic = magic
        self.pages, n_sprites, n_draws, n_grass = struct.unpack_from("<4I", data, 4)
        self.grass = n_grass
        if not (0 < n_sprites <= 4096 and 0 < n_draws <= 16384 and n_grass <= n_draws):
            raise ValueError("room table counts out of range")
        sparse = 20 + SPRITE_BYTES * n_sprites + 72 * n_draws
        expected = sparse + 24 * n_grass + (4 if magic == b"HKR4" else 0)
        if len(data) != expected:
            raise ValueError(f"room table size {len(data)} != {expected}")
        self.sprites = []
        for i in range(n_sprites):
            page, x, y, w, h, left, bottom, right, top = struct.unpack_from("<5I4f", data, 20 + SPRITE_BYTES * i)
            if page >= self.pages or w == 0 or h == 0 or x + w > 1024 or y + h > 1024 or left >= right or bottom >= top:
                raise ValueError(f"sprite {i} invalid")
            self.sprites.append((page, x, y, w, h, left, bottom, right, top))
        self.draws = []
        base = 20 + SPRITE_BYTES * n_sprites
        for i in range(n_draws):
            sprite, owner = struct.unpack_from("<2I", data, base + 72 * i)
            values = struct.unpack_from("<16f", data, base + 72 * i + 8)
            if sprite >= n_sprites or not all(math.isfinite(v) for v in values):
                raise ValueError(f"draw {i} invalid")
            self.draws.append((sprite, owner, values))
        self.tail = data[base:]  # draws, grass and split depth, copied verbatim
        self.split_depth = struct.unpack_from("<f", data, len(data) - 4)[0] if magic == b"HKR4" else 13.71


def draw_factor(sprite, v, focal: float, scale: float = 1.0):
    """PSP screen pixels per source texel (x, y) for one sprite under one draw transform."""
    depth = HERO_DEPTH + v[6]
    if depth < 10 or depth > 1000:
        return None
    _page, _x, _y, w, h, left, bottom, right, top = sprite
    ppu = focal / depth * scale
    return (ppu * math.hypot(v[0], v[3]) * (right - left) / w,
            ppu * math.hypot(v[1], v[4]) * (top - bottom) / h)


def density_factors(table: RoomTable, focal: float, swaps=None, scale_max: float = 1.0, scaled_owners=(),
                    transforms=None):
    """Largest PSP screen pixels per source texel, per sprite and axis (None: never drawn).

    swaps: {sprite: draw indices} for animation frames swapped into existing draws.
    transforms: {sprite: [16-float draw values]} for frames drawn with their own transform.
    scale_max/scaled_owners: HKS1 scale curve maximum and the owners it applies to."""
    result = [None] * len(table.sprites)

    def add(sprite_index, draw_index):
        _sprite, owner, v = table.draws[draw_index]
        f = draw_factor(table.sprites[sprite_index], v, focal, scale_max if owner in scaled_owners else 1.0)
        if f is None:
            return
        old = result[sprite_index]
        result[sprite_index] = f if old is None else (max(old[0], f[0]), max(old[1], f[1]))

    for draw_index, (sprite_index, _owner, _v) in enumerate(table.draws):
        add(sprite_index, draw_index)
    for sprite_index, draw_indices in (swaps or {}).items():
        for draw_index in draw_indices:
            add(sprite_index, draw_index)
    for sprite_index, value_list in (transforms or {}).items():
        for v in value_list:
            f = draw_factor(table.sprites[sprite_index], v, focal)
            if f is not None:
                old = result[sprite_index]
                result[sprite_index] = f if old is None else (max(old[0], f[0]), max(old[1], f[1]))
    return result


def scale_curve(data: bytes, owner_count_limit: int = 16384):
    """HKS1 (src/room_visual.cpp loadScaleAnimation): returns (maximum value, owners)."""
    if data[:4] != b"HKS1":
        raise ValueError("scale sidecar magic")
    keys, owners = struct.unpack_from("<2I", data, 4)
    if not (2 <= keys <= 256 and 0 < owners <= owner_count_limit) or len(data) != 12 + 16 * keys + 4 * owners:
        raise ValueError("scale sidecar size")
    values = [struct.unpack_from("<4f", data, 12 + 16 * i)[1] for i in range(keys)]
    # Hermite overshoot between keys is not bounded by the key values [H]; the runtime clamps
    # with its own minimum/maximum, read here as the key maximum.
    return max(values), set(struct.unpack_from(f"<{owners}I", data, 12 + 16 * keys))


def target_size(w: int, h: int, factor) -> tuple[int, int]:
    if factor is None:
        return w, h
    fx, fy = (min(1.0, f) for f in factor)
    return max(1, math.ceil(w * fx - 1e-6)), max(1, math.ceil(h * fy - 1e-6))


# ----------------------------------------------------------------- resampling
_LIN = [((c / 255) / 12.92) if c / 255 <= 0.04045 else (((c / 255) + 0.055) / 1.055) ** 2.4 for c in range(256)]
_ENC_STEPS = 4096
_ENC = [round(255 * ((x / _ENC_STEPS) * 12.92 if x / _ENC_STEPS <= 0.0031308
                     else 1.055 * (x / _ENC_STEPS) ** (1 / 2.4) - 0.055)) for x in range(_ENC_STEPS + 1)]


def resample(Image, crop, size):
    """Area downscale in linear light on premultiplied alpha; returns RGBA image."""
    if crop.size == size:
        return crop
    data = crop.tobytes()
    alpha = array("f", (a / 255 for a in data[3::4]))
    channels = []
    for c in range(3):
        lin = array("f", (_LIN[v] * a for v, a in zip(data[c::4], alpha)))
        channels.append(Image.frombytes("F", crop.size, lin.tobytes()).resize(size, Image.BOX))
    a_img = Image.frombytes("F", crop.size, alpha.tobytes()).resize(size, Image.BOX)
    a_out = array("f", a_img.tobytes())
    out = bytearray(4 * size[0] * size[1])
    for c, img in enumerate(channels):
        values = array("f", img.tobytes())
        for i, (p, a) in enumerate(zip(values, a_out)):
            out[4 * i + c] = _ENC[min(_ENC_STEPS, max(0, round(p / a * _ENC_STEPS)))] if a > 1e-6 else 0
    for i, a in enumerate(a_out):
        out[4 * i + 3] = min(255, max(0, round(a * 255)))
    return Image.frombytes("RGBA", size, bytes(out))


def blur_side(sprite, v, split):
    """roomEffectClipSprite's corner test, including its equality convention."""
    # Source static-decor banks retain collapsed placements. Their four corners
    # coincide; they cannot contribute pixels or require a blurred sprite copy.
    # Inspect depth axes too: a tilted source plane must retain its usual test.
    if all(v[i] == 0 for i in (0, 1, 3, 4, 13, 14)):
        return "empty"
    if v[13] == 0 and v[14] == 0:
        return "behind" if v[6] >= split else "front"
    depths = [v[6] + v[13] * x + v[14] * y
              for x in (sprite[5], sprite[7]) for y in (sprite[6], sprite[8])]
    return "behind" if min(depths) >= split else "front" if max(depths) <= split else "crossing"


def blur_plan(table, swaps=None, transforms=None):
    """Include runtime frame uses; never replace a sprite used by a crossing draw."""
    sides = [blur_side(table.sprites[s], v, table.split_depth) for s, _, v in table.draws]
    uses = [set() for _ in table.sprites]
    for (s, _, _), side in zip(table.draws, sides):
        uses[s].add(side)
    for s, indices in (swaps or {}).items():
        for i in indices:
            uses[s].add(blur_side(table.sprites[s], table.draws[i][2], table.split_depth))
    for s, values in (transforms or {}).items():
        for v in values:
            uses[s].add(blur_side(table.sprites[s], v, table.split_depth))
    mapping, count = {}, len(table.sprites)
    for s, use in enumerate(uses):
        if "behind" in use:
            mapping[s] = s if use == {"behind"} else count
            count += use != {"behind"}
    tail = bytearray(table.tail)
    for i, ((s, _, _), side) in enumerate(zip(table.draws, sides)):
        if side == "behind":
            struct.pack_into("<I", tail, 72 * i, mapping[s])
    counts = {s: sides.count(s) for s in ("behind", "front", "crossing")}
    if "empty" in sides:
        counts["empty"] = sides.count("empty")
    return mapping, bytes(tail), counts


def behind_factors(table, focal, swaps, transforms, scale_max, scaled):
    """Use the blurred copy's draws, rather than a larger sharp foreground use."""
    class Behind:
        sprites = table.sprites
        draws = [(s, o, v) for s, o, v in table.draws
                 if blur_side(table.sprites[s], v, table.split_depth) == "behind"]
    remap = {old: new for new, old in enumerate(i for i, (s, _, v) in enumerate(table.draws)
             if blur_side(table.sprites[s], v, table.split_depth) == "behind")}
    swaps = {s: {remap[i] for i in ids if i in remap} for s, ids in swaps.items()}
    transforms = {s: [v for v in values if blur_side(table.sprites[s], v, table.split_depth) == "behind"]
                  for s, values in transforms.items()}
    return density_factors(Behind, focal, swaps, scale_max, scaled, transforms)


def gamma_channels(Image, image):
    """Floating premultiplied sRGB, avoiding coloured transparent fringes."""
    data = image.tobytes()
    alpha = array("f", (a / 255 for a in data[3::4]))
    return [Image.frombytes("F", image.size, array("f", (v * a for v, a in
            zip(data[c::4], alpha))).tobytes()) for c in range(3)] + [
                Image.frombytes("F", image.size, array("f", (255 * a for a in alpha)).tobytes())]


def gamma_image(Image, channels):
    values = [array("f", ch.tobytes()) for ch in channels]
    out = bytearray(4 * len(values[0]))
    for i, alpha in enumerate(values[3]):
        out[4*i+3] = min(255, max(0, round(alpha)))
        if alpha > 1e-6:
            for c in range(3):
                out[4*i+c] = min(255, max(0, round(values[c][i] * 255 / alpha)))
    return Image.frombytes("RGBA", channels[0].size, bytes(out))


def gamma_resize(Image, image, size, method):
    return gamma_image(Image, [ch.resize(size, method) for ch in gamma_channels(Image, image)])


def bake_blur(Image, crop, sprite, factor):
    """Exact four bilinear tap passes in source texels, with transparent support.

    [H] Constant density per sprite approximates perspective/animated scale.
    Geometry expands symmetrically; the asymmetric kernel's drift is retained.
    """
    from PIL import ImageMath
    spacing = tuple(BLUR_PSP_SPACING / f for f in factor)
    margin = tuple(math.ceil(2 * max(map(abs, BLUR_OFFSETS)) * step) + 2 for step in spacing)
    mx, my = margin
    padded = Image.new("RGBA", (crop.width + 2*mx, crop.height + 2*my))
    padded.paste(crop, (mx, my))
    channels = gamma_channels(Image, padded)
    for axis in (0, 1, 0, 1):
        for c, channel in enumerate(channels):
            samples = {}
            for k, (offset, weight) in enumerate(zip(BLUR_OFFSETS, BLUR_WEIGHTS)):
                delta = offset * spacing[axis]
                # Unity UV Y runs opposite sprite-image Y; keep the source asymmetry.
                dx, dy = (delta, 0) if axis == 0 else (0, -delta)
                samples[f"s{k}"] = channel.transform(channel.size, Image.Transform.AFFINE,
                    (1, 0, dx, 0, 1, dy), Image.Resampling.BILINEAR, fillcolor=0)
                samples[f"w{k}"] = weight
            channels[c] = ImageMath.unsafe_eval("s0*w0+s1*w1+s2*w2+s3*w3+s4*w4", **samples)
    channels[:3] = [ImageMath.unsafe_eval("c*v", c=c, v=1+BLUR_VIBRANCE) for c in channels[:3]]
    full = gamma_image(Image, channels)
    full = resample(Image, full, target_size(full.width, full.height, factor))
    chosen, scale, psnr = full, 1, 99.0
    while min(chosen.size) > 1:
        size = (max(1, chosen.width // 2), max(1, chosen.height // 2))
        candidate = gamma_resize(Image, full, size, Image.Resampling.BOX)
        restored = gamma_resize(Image, candidate, full.size, Image.Resampling.BILINEAR)
        quality = premultiplied_psnr(full.tobytes(), restored.tobytes())
        if quality < 40:
            break
        chosen, scale, psnr = candidate, scale * 2, quality
    left, bottom, right, top = sprite[5:]
    ux, uy = (right-left)/crop.width, (top-bottom)/crop.height
    geometry = (left-mx*ux, bottom-my*uy, right+mx*ux, top+my*uy)
    return chosen, geometry, {"margin_source_texels": list(margin), "reduction": scale,
                              "psnr_db": round(psnr, 3), "size": list(chosen.size)}


# -------------------------------------------------------------------- packing
def pack(sizes, page=PAGE, pad=PAD):
    """Shelf packer: returns [(page, x, y)] for (w, h) sizes; padding on every side."""
    order = sorted(range(len(sizes)), key=lambda i: (-sizes[i][1], -sizes[i][0], i))
    places = [None] * len(sizes)
    pages = []  # per page: list of shelves [y, height, next_x]
    for i in order:
        w, h = sizes[i][0] + 2 * pad, sizes[i][1] + 2 * pad
        if w > page or h > page:
            raise ValueError(f"sprite {i} {sizes[i]} does not fit a {page} page")
        done = False
        for p, shelves in enumerate(pages):
            for shelf in shelves:
                if h <= shelf[1] and shelf[2] + w <= page:
                    places[i] = (p, shelf[2] + pad, shelf[0] + pad)
                    shelf[2] += w
                    done = True
                    break
            if not done:
                top = shelves[-1][0] + shelves[-1][1] if shelves else 0
                if top + h <= page:
                    shelves.append([top, h, w])
                    places[i] = (p, pad, top + pad)
                    done = True
            if done:
                break
        if not done:
            pages.append([[0, h, w]])
            places[i] = (len(pages) - 1, pad, pad)
    return places, len(pages)


def paste_padded(page_img, img, x, y, pad=PAD):
    page_img.paste(img, (x, y))
    w, h = img.size
    for d in range(1, pad + 1):  # clamp edges so bilinear filtering never reads a neighbour
        page_img.paste(img.crop((0, 0, w, 1)), (x, y - d))
        page_img.paste(img.crop((0, h - 1, w, h)), (x, y + h - 1 + d))
        page_img.paste(page_img.crop((x, y - pad, x + 1, y + h + pad)), (x - d, y - pad))
        page_img.paste(page_img.crop((x + w - 1, y - pad, x + w, y + h + pad)), (x + w - 1 + d, y - pad))


# ------------------------------------------------------------------ PSP texture
def swizzle(data: bytes, row_bytes: int, height: int) -> bytes:
    """PSP GE swizzle: 16-byte x 8-row blocks, row-major over blocks."""
    if row_bytes % 16 or height % 8:
        raise ValueError("swizzle needs row bytes % 16 == 0 and height % 8 == 0")
    out = bytearray()
    for by in range(0, height, 8):
        for bx in range(0, row_bytes, 16):
            for r in range(8):
                start = (by + r) * row_bytes + bx
                out += data[start:start + 16]
    return bytes(out)


def unswizzle(data: bytes, row_bytes: int, height: int) -> bytes:
    out = bytearray(len(data))
    i = 0
    for by in range(0, height, 8):
        for bx in range(0, row_bytes, 16):
            for r in range(8):
                start = (by + r) * row_bytes + bx
                out[start:start + 16] = data[i:i + 16]
                i += 16
    return bytes(out)


def hpt1(width: int, height: int, palette_rgba: bytes, indices: bytes) -> bytes:
    if len(palette_rgba) != 1024 or len(indices) != width * height:
        raise ValueError("bad CLUT8 texture data")
    header = b"HPT1" + struct.pack("<HHBBBBI", width, height, GU_PSM_T8, GU_PSM_8888, 1, 0, 256)
    return header + palette_rgba + swizzle(indices, width, height)


def premultiplied_psnr(a: bytes, b: bytes) -> float:
    total = 0
    for i in range(0, len(a), 4):
        aa, ab = a[i + 3], b[i + 3]
        total += (aa - ab) ** 2
        for c in range(3):
            total += ((a[i + c] * aa - b[i + c] * ab) / 255) ** 2
    mse = total / len(a)
    return 99.0 if mse == 0 else 10 * math.log10(255 * 255 / mse)


# ------------------------------------------------------------------------ main
def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def quantize_page(Image, image):
    """Weighted median-cut in premultiplied sRGB + alpha, without dithering.

    FASTOCTREE's coarse RGBA buckets lose dark ramps even with spare colours.
    Split by weighted variance instead, retaining full 8-bit dark precision.
    Alpha participates in both palette construction and nearest-colour lookup;
    hidden RGB never consumes palette entries. All work is offline.
    """
    weights = (2, 3, 1, 2)  # gamma-space luminance emphasis plus alpha coverage

    def premul(c):
        r, g, b, a = c
        return ((r*a+127)//255, (g*a+127)//255, (b*a+127)//255, a)

    colours = image.getcolors(image.width * image.height)
    histogram = {}
    for count, colour in colours:
        p = premul(colour)
        if p[3]:
            histogram[p] = histogram.get(p, 0) + count
    points = sorted(histogram)

    def box(ids):
        count = sum(histogram[points[i]] for i in ids)
        sums = [sum(points[i][c] * histogram[points[i]] for i in ids) for c in range(4)]
        variance = [weights[c] * (sum(points[i][c]**2 * histogram[points[i]] for i in ids)
                                 - sums[c]**2 / count) for c in range(4)]
        return sum(variance), max(range(4), key=lambda c: variance[c]), count, sums

    boxes, heap = {}, []
    def add(ids):
        serial = len(boxes)
        stats = box(ids)
        boxes[serial] = (ids, stats)
        if len(ids) > 1:
            heapq.heappush(heap, (-stats[0], serial))

    if points:
        add(list(range(len(points))))
    leaves = set(boxes)
    while len(leaves) < 255 and heap:
        _, serial = heapq.heappop(heap)
        ids, (_, axis, count, _) = boxes[serial]
        ids = sorted(ids, key=lambda i: (points[i][axis], points[i]))
        mass, split = 0, 1
        for split in range(1, len(ids)):
            mass += histogram[points[ids[split-1]]]
            if mass * 2 >= count:
                break
        leaves.remove(serial)
        for part in (ids[:split], ids[split:]):
            add(part)
            leaves.add(len(boxes)-1)
    palette = [(0, 0, 0, 0)]  # exact transparent padding, including empty pages
    for serial in sorted(leaves):
        _, (_, _, count, sums) = boxes[serial]
        alpha = max(1, round(sums[3] / count))
        palette.append(tuple(min(255, max(0, round(sums[c] * 255 / sums[3])))
                             for c in range(3)) + (alpha,))
    encoded = [premul(c) for c in palette]

    def tree(ids):
        if not ids:
            return None
        axis = max(range(4), key=lambda c: weights[c] *
                   (max(encoded[i][c] for i in ids)-min(encoded[i][c] for i in ids))**2)
        ids = sorted(ids, key=lambda i: (encoded[i][axis], i))
        middle = len(ids)//2
        return ids[middle], axis, tree(ids[:middle]), tree(ids[middle+1:])

    search = tree(list(range(len(palette))))
    def nearest(p):
        best, distance = 0, float('inf')
        def visit(node):
            nonlocal best, distance
            if node is None:
                return
            i, axis, left, right = node
            q = encoded[i]
            d = sum(w*(x-y)**2 for w, x, y in zip(weights, p, q))
            if d < distance or (d == distance and i < best):
                best, distance = i, d
            delta = p[axis]-q[axis]
            near, far = (left, right) if delta < 0 else (right, left)
            visit(near)
            if weights[axis]*delta*delta <= distance:
                visit(far)
        visit(search)
        return best

    indices = {p: nearest(p) for p in [(0, 0, 0, 0), *points]}
    mapping = {bytes(c): indices[premul(c)] for _, c in colours}
    data = image.tobytes()
    quant = Image.frombytes('P', image.size,
                            bytes(mapping[data[i:i+4]] for i in range(0, len(data), 4)))
    rgba = bytes(v for c in palette for v in c)
    quant.putpalette(rgba + bytes(1024-len(rgba)), rawmode='RGBA')
    return quant


def build_tiled_pages(Image, package, page_pattern, source_pages, sprites, factors, out, baked, tile_records,
                      sampler=resample, extra_pages=None):
    """Keep logical sprite identities; tile texels and local geometry together.

    Internal padding samples the adjacent source texel, so linear filtering has
    the same footprint on both sides of a tile boundary.
    """
    sources = {n: Image.open(package / page_pattern.format(n=n)).convert("RGBA") for n in range(source_pages)}
    sources.update(extra_pages or {})
    images, geometries = [], []
    for index in range(max(len(sprites), max(baked or {}, default=-1)+1)):
        if index in (baked or {}):
            image, geometry, _ = baked[index]
        else:
            page, x, y, w, h, *geometry = sprites[index]
            image = sampler(Image, sources[page].crop((x, y, x+w, y+h)),
                             target_size(w, h, factors[index]))
        images.append(image)
        geometries.append(geometry)
    del sources
    chunks, owners = [], []
    limit = PAGE-2*PAD
    for index, image in enumerate(images):
        for y in range(0, image.height, limit):
            for x in range(0, image.width, limit):
                w, h = min(limit, image.width-x), min(limit, image.height-y)
                chunks.append((w, h))
                owners.append((index, x, y))
    places, page_count = pack(chunks)
    pages = [Image.new("RGBA", (PAGE, PAGE)) for _ in range(page_count)]
    padded = []
    for image in images:
        edge = Image.new("RGBA", (image.width+2*PAD, image.height+2*PAD))
        paste_padded(edge, image, PAD, PAD)
        padded.append(edge)
    frames = [[] for _ in images]
    for (w, h), (index, x, y), (page, px, py) in zip(chunks, owners, places):
        image = images[index]
        pages[page].paste(padded[index].crop((x, y, x+w+2*PAD, y+h+2*PAD)), (px-PAD, py-PAD))
        left, bottom, right, top = geometries[index]
        geometry = (left+(right-left)*x/image.width, top-(top-bottom)*(y+h)/image.height,
                    left+(right-left)*(x+w)/image.width, top-(top-bottom)*y/image.height)
        frames[index].append((x, y, page, px, py, w, h, *geometry))
    sizes, table = [], bytearray()
    for index, (image, parts) in enumerate(zip(images, frames)):
        sizes.append(image.size)
        if len(parts) == 1:
            table += struct.pack("<5I4f", *parts[0][2:])
        else:
            table += struct.pack("<5I4f", 0, 0, 0, image.width, image.height, *geometries[index])
            tile_records.append((index, parts))
    out.mkdir(parents=True, exist_ok=True)
    stats = []
    for n, image in enumerate(pages):
        quant = quantize_page(Image, image)
        palette = bytes(quant.getpalette(rawmode="RGBA") or [])
        (out / f"page-{n}.hpt").write_bytes(hpt1(PAGE, PAGE, palette+bytes(1024-len(palette)), quant.tobytes()))
        back = quant.convert("RGBA")
        back.save(out / f"page-{n}.png")
        stats.append({"page": n, "psnr_db": round(premultiplied_psnr(image.tobytes(), back.tobytes()), 2)})
    return sizes, page_count, stats, bytes(table)


def build_pages(Image, package: Path, page_pattern: str, source_pages: int, sprites, factors, out: Path,
                baked=None, tile_records=None, sampler=resample, extra_pages=None):
    """Resample, pack, quantise and write page-N.hpt/png; returns sizes, page count, PSNR, sprite table."""
    sizes = [target_size(s[3], s[4], f) for s, f in zip(sprites, factors)]
    if tile_records is not None and (any(max(size)+2*PAD>PAGE for size in sizes) or
                                     any(max(value[0].size)+2*PAD>PAGE for value in (baked or {}).values())):
        return build_tiled_pages(Image, package, page_pattern, source_pages, sprites, factors,
                                 out, baked, tile_records, sampler, extra_pages)
    places, page_count = pack(sizes)
    sources = {n: Image.open(package / page_pattern.format(n=n)).convert("RGBA") for n in range(source_pages)}
    sources.update(extra_pages or {})
    pages = [Image.new("RGBA", (PAGE, PAGE), (0, 0, 0, 0)) for _ in range(page_count)]
    for sprite, size, place in zip(sprites, sizes, places):
        page, x, y, w, h = sprite[:5]
        img = sampler(Image, sources[page].crop((x, y, x + w, y + h)), size)
        paste_padded(pages[place[0]], img, place[1], place[2])
    out.mkdir(parents=True, exist_ok=True)
    page_stats, quantized = [], []
    for n, img in enumerate(pages):
        quant = quantize_page(Image, img)
        quantized.append(quant)
        back = quant.convert("RGBA")
        page_stats.append({"page": n, "psnr_db": round(premultiplied_psnr(img.tobytes(), back.tobytes()), 2)})
    geometries = [s[5:] for s in sprites]
    if baked:
        # Retain baseline palettes/rectangles exactly for every sharp sprite.
        keep = sorted({places[i][0] for i in range(len(sprites)) if i not in baked})
        remap = {old: new for new, old in enumerate(keep)}
        quantized = [quantized[i] for i in keep]
        page_stats = [dict(page_stats[i], page=n) for n, i in enumerate(keep)]
        places = [(remap.get(p, 0), x, y) for p, x, y in places]
        indices = sorted(baked)
        locations, extra_count = pack([baked[i][0].size for i in indices])
        extra = [Image.new("RGBA", (PAGE, PAGE)) for _ in range(extra_count)]
        for i, (p, x, y) in zip(indices, locations):
            img, geometry, _ = baked[i]
            paste_padded(extra[p], img, x, y)
            if i == len(sizes):
                sizes.append(img.size)
                places.append((len(keep)+p, x, y))
                geometries.append(geometry)
            else:
                sizes[i], places[i], geometries[i] = img.size, (len(keep)+p, x, y), geometry
        for img in extra:
            quant = quantize_page(Image, img)
            page_stats.append({"page": len(quantized), "psnr_db": round(
                premultiplied_psnr(img.tobytes(), quant.convert("RGBA").tobytes()), 2)})
            quantized.append(quant)
        page_count = len(quantized)
    for n, quant in enumerate(quantized):
        palette = quant.getpalette(rawmode="RGBA") or []
        palette = bytes(palette) + bytes(1024 - len(palette))
        (out / f"page-{n}.hpt").write_bytes(hpt1(PAGE, PAGE, palette, quant.tobytes()))
        back = quant.convert("RGBA")
        back.save(out / f"page-{n}.png")
    sprite_bytes = bytearray()
    for geometry, size, place in zip(geometries, sizes, places):
        sprite_bytes += struct.pack("<5I4f", place[0], place[1], place[2], size[0], size[1], *geometry)
    return sizes, page_count, page_stats, bytes(sprite_bytes)


# ------------------------------------------------------------ animation atlas
class AnimationTable:
    """HKA1 (src/animation.cpp AnimationBank::load): pages, sprites (36 B), clips (68 B), frames (u32)."""

    def __init__(self, data: bytes):
        if len(data) < 20:
            raise ValueError("animation table truncated")
        if data[:4] != b"HKA1":
            raise ValueError("animation table magic")
        self.pages, n_sprites, n_clips, n_frames = struct.unpack_from("<4I", data, 4)
        if not (0 < self.pages <= 32 and 0 < n_sprites <= 8192 and 0 < n_clips <= 512 and 0 < n_frames <= 65536):
            raise ValueError("animation table counts out of range")
        end = 20 + 36 * n_sprites + 68 * n_clips + 4 * n_frames
        if len(data) < end:
            raise ValueError("animation table size")
        from animation_library import parse_events
        self.event_bytes = data[end:]
        self.events = parse_events(self.event_bytes, n_frames)
        self.sprites = [struct.unpack_from("<5I4f", data, 20 + 36 * i) for i in range(n_sprites)]
        for i, (page, x, y, w, h, left, bottom, right, top) in enumerate(self.sprites):
            if (page >= self.pages or not w or not h or x + w > 1024 or y + h > 1024 or
                    not all(map(math.isfinite, (left, bottom, right, top))) or right <= left or top <= bottom):
                raise ValueError(f"animation sprite {i} invalid")
        self.clip_bytes = data[20 + 36 * n_sprites:20 + 36 * n_sprites + 68 * n_clips]
        base = 20 + 36 * n_sprites + 68 * n_clips
        self.frames = struct.unpack_from(f"<{n_frames}I", data, base)
        if max(self.frames) >= n_sprites:
            raise ValueError("animation frame out of range")
        self.n_clips = n_clips
        self.clips = {}
        for offset in range(0, len(self.clip_bytes), 68):
            raw, fps, wrap, loop, first, count = struct.unpack_from("<48sf4I", self.clip_bytes, offset)
            if (b"\0" not in raw or not math.isfinite(fps) or fps <= 0 or not count or
                    first > n_frames or count > n_frames - first or
                    (wrap == 1 and loop >= count) or wrap not in (0, 1, 2, 3, 6)):
                raise ValueError("animation clip invalid")
            # AnimationBank::find returns the first match.
            self.clips.setdefault(raw.split(b"\0", 1)[0], self.frames[first:first + count])


def atlas_factors(table, focal, effects):
    """Combine hero-plane and effect transforms; effects also cover non-clip frames."""
    used = set(table.frames)
    identity = [1.0, 0, 0, 0, 1.0, 0] + [0.0] * 10
    factors = []
    for i, sprite in enumerate(table.sprites):
        if i not in used and i not in effects:
            factors.append(None)
            continue
        base = draw_factor(sprite, identity, focal)
        scale = effects.get(i, (1, 1))
        factors.append(tuple(min(1.0, value * max(1.0, s)) for value, s in zip(base, scale)))
    return factors


def transcode_atlas(package: Path, table_name: str, page_pattern: str, out: Path, name: str) -> dict:
    """Atlas density at z=0, including the maximum hero effect transforms."""
    Image = import_pil()
    table = AnimationTable((package / table_name).read_bytes())
    focal = room_focal_length(PSP_WIDTH, PSP_HEIGHT)
    import atlas_sidecars
    effects, effect_sources, missing_effects = {}, {}, []
    if name == "hero":
        for filename, reader in (("hero-effects.bin", atlas_sidecars.hero_effects),
                                 ("focus-effects.bin", atlas_sidecars.focus_effects)):
            path = package / filename
            if not path.exists():
                missing_effects.append(f"missing effect sidecar: {filename}")
                continue
            for sprite, scale in reader(path.read_bytes(), len(table.sprites), table.clips).items():
                previous = effects.get(sprite, (0, 0))
                effects[sprite] = tuple(max(a, b) for a, b in zip(previous, scale))
            effect_sources[filename] = sha256(path)
        # Legacy synthetic/older packages can omit HKN1. Preserve their explicit
        # incomplete marker; real Tutorial packages include each variant's scale.
        path = package / "nail.bin"
        if path.exists():
            for sprite, scale in atlas_sidecars.nail_shapes(path.read_bytes(), len(table.sprites), table.clips).items():
                previous = effects.get(sprite, (0, 0))
                effects[sprite] = tuple(max(a, b) for a, b in zip(previous, scale))
            effect_sources["nail.bin"] = sha256(path)
        if b"Death Flash" in table.clips and b"Death Flash Rays" in table.clips:
            path = REPO / "src/hero_death.cpp"
            for sprite, scale in atlas_sidecars.death_flash_scales(path.read_text(), len(table.sprites), table.clips).items():
                previous = effects.get(sprite, (0, 0))
                effects[sprite] = tuple(max(a, b) for a, b in zip(previous, scale))
            effect_sources["src/hero_death.cpp"] = sha256(path)
    factors = atlas_factors(table, focal, effects)
    sizes, page_count, page_stats, sprite_bytes = build_pages(Image, package, page_pattern, table.pages,
                                                              table.sprites, factors, out)
    header = b"PSA1" + struct.pack("<6I", PSR_VERSION, PAGE, page_count, len(table.sprites), table.n_clips,
                                   len(table.frames))
    (out / f"{Path(table_name).stem}.psp.bin").write_bytes(
        header + sprite_bytes + table.clip_bytes + struct.pack(f"<{len(table.frames)}I", *table.frames))
    source_texels = sum(s[3] * s[4] for s in table.sprites)
    psp_texels = sum(w * h for w, h in sizes)
    manifest = {
        "atlas": name, "format": "PSA1", "version": PSR_VERSION, "focal_px": round(focal, 3),
        "quantizer": "weighted-median-cut-premultiplied-srgb-alpha-no-dither",
        "page_size": PAGE, "pages": page_count, "texture_bytes": page_count * (PAGE * PAGE + 1024),
        "sprites": len(table.sprites), "clips": table.n_clips,
        "effect_sprites": len(effects),
        "max_effect_scale": [max((s[axis] for s in effects.values()), default=0) for axis in (0, 1)],
        "effect_scales": {str(i): list(s) for i, s in sorted(effects.items())},
        "factors": [None if f is None else list(f) for f in factors],
        "source_size_sprites": [i for i, f in enumerate(factors) if f is None],
        "source_texels": source_texels, "psp_texels": psp_texels,
        "texel_ratio": round(psp_texels / source_texels, 4), "page_psnr": page_stats,
        "source_sha256": {table_name: sha256(package / table_name), **effect_sources,
                          **{page_pattern.format(n=n): sha256(package / page_pattern.format(n=n))
                             for n in range(table.pages)}},
        "not_done": missing_effects + ([] if "nail.bin" in effect_sources else ["nail HKN1"]) + ["hero scale other than 1 [H]"],
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=1), encoding="utf-8")
    return manifest


def switch_sprite_uses(data, table, draw_remap=None):
    """Yield (sprite field offset, sprite, draw) in an MGA1 room switch bank."""
    if len(data) < 8 or data[:4] != b"MGA1":
        raise ValueError("Invalid MGA1 header")
    count, = struct.unpack_from("<I", data, 4)
    if not 0 < count <= 64:
        raise ValueError("Invalid MGA1 count")
    empty = 0xffffffff
    offset = 8
    for _ in range(count):
        if offset + 48 > len(data):
            raise ValueError("Truncated MGA1 animation")
        _trigger, slots, frames = struct.unpack_from("<3I", data, offset)
        draws = struct.unpack_from("<4I", data, offset + 16)
        end = offset + 48 + 20 * frames
        if not (0 < slots <= 4 and 0 < frames <= 256 and end <= len(data)
                and all(draw < (len(table.draws) if draw_remap is None else len(draw_remap)) for draw in draws[:slots])
                and all(draw == empty for draw in draws[slots:])):
            raise ValueError("Invalid MGA1 animation")
        if draw_remap is not None:
            draws = tuple(draw_remap[draw] for draw in draws[:slots]) + draws[slots:]
            struct.pack_into("<4I", data, offset + 16, *draws)
        for start in (offset + 32, *range(offset + 52, end, 20)):
            for slot, sprite in enumerate(struct.unpack_from("<4I", data, start)):
                if sprite == empty:
                    continue
                if slot >= slots or sprite >= len(table.sprites):
                    raise ValueError("Invalid MGA1 sprite")
                yield start + 4 * slot, sprite, draws[slot]
        offset = end
    if offset != len(data):
        raise ValueError("Unexpected MGA1 trailer")


WHITE_WAVE = "White Wave"
WHITE_WAVE_PARENT = "Shiny Item"


def _object_dump(scene_dir: Path, level: str, path_id: int):
    path = scene_dir / f"level{level}-{path_id}.json"
    return json.loads(path.read_text(encoding="utf-8")) if path.is_file() else None


def white_wave_owners(research: Path | None, scene: str, owners) -> set[int]:
    """Owner GameObjects named "White Wave" whose parent name starts with "Shiny Item".

    Found by name and hierarchy in the HK3DS research object dumps (read-only):
    research/<scene>/visuals/level<N>-<pathID>.json. Scenes without a dump return nothing.
    """
    scene_dir = research / scene / "visuals" if research is not None else None
    if scene_dir is None or not scene_dir.is_dir():
        return set()
    levels = {m.group(1) for p in scene_dir.glob("level*-*.json")
              if (m := re.fullmatch(r"level(\d+)-\d+\.json", p.name))}
    if len(levels) != 1:
        raise ValueError(f"{scene}: expected one level prefix in {scene_dir}, found {sorted(levels)}")
    level, = levels
    hidden = set()
    for owner in sorted(set(owners)):
        game_object = _object_dump(scene_dir, level, owner)
        if not game_object or game_object.get("m_Name") != WHITE_WAVE:
            continue
        parent_name = None
        for component in game_object["m_Components"]:
            transform = _object_dump(scene_dir, level, component["m_Component"]["m_PathID"])
            if transform and "m_Father" in transform and transform["m_Father"]["m_PathID"]:
                father = _object_dump(scene_dir, level, transform["m_Father"]["m_PathID"])
                parent = _object_dump(scene_dir, level, father["m_GameObject"]["m_PathID"]) if father else None
                parent_name = parent.get("m_Name") if parent else None
        if parent_name is not None and parent_name.startswith(WHITE_WAVE_PARENT):
            hidden.add(owner)
    return hidden


def hide_draws(tail: bytes, indices) -> bytes:
    """Set opacity (values[10]) to 0 on draws in place: roomDrawHasOpacity rejects them, so no quad
    is emitted, and the draw count, order and the grass/sidecar indices stay unchanged."""
    out = bytearray(tail)
    for i in indices:
        struct.pack_into("<f", out, 72 * i + 8 + 4 * 10, 0.0)
    return bytes(out)


def mantis_arena(Image, table, source):
    """Restore the active Mantis Battle overlay omitted by the base room package."""
    sys.path.insert(0, str(REPO / "tools"))
    from convert_room_visuals import additive_owner_id, decode, scene_ambient, sprite_tint
    from room_startup import project_with_startup, world_translations

    def read(name):
        if name not in objects:
            objects[name] = json.loads((source / name).read_bytes())
        return objects[name]

    def painter(draw):
        values = draw[2]
        return values[11], values[12], -values[6]

    objects = {}
    mapping = read("room-visuals-map.json")
    if mapping["scene"] != "Fungus2_15_boss" or read("level180-16.json")["m_Name"] != "Mantis Battle":
        raise ValueError("Unexpected Mantis Battle export")
    offsets = {id(read(name)): value for name, value in world_translations(mapping, read).items()}
    ambient = mapping.get("scene_ambient_rgb") or scene_ambient()
    existing = {draw[1] for draw in table.draws}
    owners, pages, sprites, textures, draws = set(), {}, {}, {}, []
    for item in mapping["inventory"]:
        if not item["active"] or "level180-16.json" not in item["owners"]:
            continue
        owner = additive_owner_id(int(item["go"].split("-")[-1].split(".")[0]))
        if owner in existing or owner in owners:
            raise ValueError(f"Duplicate Mantis arena owner {owner:#x}")
        owners.add(owner)
        key = item["sprite"]
        if key not in sprites:
            texture = mapping["sprites"][key]
            if texture not in textures:
                textures[texture] = Image.open(source / texture).convert("RGBA")
            image, geometry = decode(textures[texture], read(key))
            page = table.pages + len(pages)
            pages[page] = image
            sprites[key] = len(table.sprites)
            table.sprites.append((page, 0, 0, *image.size, *geometry))
        chain = [read(name) for name in item["chain"]]
        origin, unit_x, unit_y = (project_with_startup(point, chain, offsets)
                                  for point in ((0, 0, 0), (1, 0, 0), (0, 1, 0)))
        renderer = read(item["renderer"])
        axis_x = tuple((value - base) * (-1 if renderer["m_FlipX"] else 1) for value, base in zip(unit_x, origin))
        axis_y = tuple((value - base) * (-1 if renderer["m_FlipY"] else 1) for value, base in zip(unit_y, origin))
        materials = [read(name) for name in item["materials"]]
        if any(material["m_Name"] not in ("Sprites-Default", "Crossroads_Material") for material in materials):
            raise ValueError(f"Unsupported Mantis arena material for {owner:#x}")
        values = (axis_x[0], axis_y[0], origin[0], axis_x[1], axis_y[1], origin[1], origin[2],
                  *sprite_tint(renderer["m_Color"], materials, ambient), renderer["m_SortingLayer"],
                  renderer["m_SortingOrder"], axis_x[2], axis_y[2], 0)
        draws.append((sprites[key], owner, struct.unpack("<16f", struct.pack("<16f", *values))))
    if len(owners) != 34 or not {additive_owner_id(n) for n in (7, 8, 19, 20)} <= owners:
        raise ValueError("Incomplete Mantis arena overlay")
    if any(painter(left) > painter(right) for left, right in zip(table.draws, table.draws[1:])):
        raise ValueError("Unsorted base Mantis arena draws")
    count = len(table.draws)
    merged = sorted(enumerate(table.draws + draws), key=lambda entry: painter(entry[1]))
    remap = [0] * count
    tail = bytearray(table.tail[72 * count:])
    for new, (old, _) in enumerate(merged):
        if old < count:
            remap[old] = new
    for offset in range(0, len(tail) - (4 if table.magic == b"HKR4" else 0), 24):
        old, = struct.unpack_from("<I", tail, offset)
        struct.pack_into("<I", tail, offset, remap[old])
    table.draws = [draw for _, draw in merged]
    table.tail = b"".join(struct.pack("<2I16f", sprite, owner, *values) for sprite, owner, values in table.draws) + tail
    return pages, remap


MATO_SCENE = "Room_nailmaster"


def mato_inputs(root):
    original = root / "_private/original/Hollow Knight/hollow_knight_Data"
    visuals = root / "_private/research/crossroads-scenes" / MATO_SCENE / "visuals"
    return (original / "sharedassets13.assets", original / "sharedassets13.assets.resS",
            original / "Managed/Assembly-CSharp.dll", *(visuals / f"level13-{pid}.json"
            for pid in (59, 276, 442, 682, 687)))


def mato_rest(table, root):
    """Bake the original one-frame Rest clip; NPC interactions are not part of the room bake."""
    sys.path.insert(0, str(root / "_private/tools/boss-python"))
    sys.path.insert(0, str(REPO / "tools"))
    from UnityPy import Environment
    from watcher_unity import AlignedGenerator
    from convert_hero import decode_sprite

    asset, stream, assembly, *records = mato_inputs(root)
    owner, transform, renderer, sprite, animator = (json.loads(path.read_bytes()) for path in records)
    sprite, animator = sprite["m_Structure"], animator["m_Structure"]
    if (owner["m_Name"], sprite["collection"], animator["library"], transform["m_Father"]["m_PathID"],
            renderer["m_Enabled"]) != ("NM Mato NPC", {"m_FileID": 4, "m_PathID": 120},
            {"m_FileID": 4, "m_PathID": 122}, 0, True):
        raise ValueError("Changed Mato source owner")
    rotation = transform["m_LocalRotation"]
    if (rotation["m_W"], rotation["m_X"], rotation["m_Y"], rotation["m_Z"]) != (1, 0, 0, 0):
        raise ValueError("Changed Mato rotation")
    env = Environment(path=str(asset.parent))
    generator = AlignedGenerator("6000.0.61f1")
    generator.load_local_dll_folder(str(assembly.parent))
    env.typetree_generator = generator
    env.load_file(str(stream))
    objects = env.load_file(str(asset)).objects
    clip = objects[122].read_typetree()["clips"][animator["defaultClipId"]]
    if clip["name"] != "Rest" or len(clip["frames"]) != 1:
        raise ValueError("Changed Mato Rest clip")
    frame = clip["frames"][0]
    if frame["spriteCollection"] != {"m_FileID": 0, "m_PathID": 120} or frame["spriteId"] != sprite["_spriteId"]:
        raise ValueError("Changed Mato Rest frame")
    collection = objects[120].read_typetree()
    definition = collection["spriteDefinitions"][frame["spriteId"]]
    texture = collection["textures"][definition["materialId"]]
    if texture["m_FileID"] != 0 or definition["materialId"] != 0:
        raise ValueError("Changed Mato texture reference")
    image, bounds = decode_sprite(objects[texture["m_PathID"]].read().image.convert("RGBA"), definition)
    position, scale = transform["m_LocalPosition"], transform["m_LocalScale"]
    values = (scale["m_X"] * sprite["_scale"]["x"], 0, position["m_X"],
              0, scale["m_Y"] * sprite["_scale"]["y"], position["m_Y"], position["m_Z"],
              *(sprite["_color"][channel] for channel in "rgba"),
              renderer["m_SortingLayer"], renderer["m_SortingOrder"], 0, 0, 0)
    append_mato_draw(table, image.size, bounds, values)
    return {table.pages: image}


def append_mato_draw(table, size, bounds, values):
    if table.grass or any(owner == 59 for _, owner, _ in table.draws):
        raise ValueError("Mato room has indexed grass or already contains owner 59")
    sprite = len(table.sprites)
    table.sprites.append((table.pages, 0, 0, *size, *bounds))
    packed = struct.pack("<2I16f", sprite, 59, *values)
    values = struct.unpack_from("<16f", packed, 8)
    painter = lambda pose: (pose[11], pose[12], -pose[6])
    index = next((i for i, (_, _, pose) in enumerate(table.draws) if painter(pose) > painter(values)), len(table.draws))
    table.draws.insert(index, (sprite, 59, values))
    offset = 72 * index
    table.tail = table.tail[:offset] + packed + table.tail[offset:]


def transcode(package: Path, visual: str, page_pattern: str, out: Path, scene: str, sidecars=None,
              sidecar_package: Path | None = None, research: Path | None = None) -> dict:
    """sidecars: {'plants'|'dynamics'|'ambient': file name in the package}.
    research: HK3DS _private/research/crossroads-scenes (object names for hidden draws)."""
    from room_sidecars import sprite_swaps, baked_sidecars
    Image = import_pil()
    table = RoomTable((package / visual).read_bytes())
    focal = room_focal_length(PSP_WIDTH, PSP_HEIGHT)
    switch_path = package / (visual + ".switches")
    switches = bytearray(switch_path.read_bytes()) if switch_path.is_file() else None
    scale_max, scaled = 1.0, set()
    if (package / (visual + ".scale")).exists():
        scale_max, scaled = scale_curve((package / (visual + ".scale")).read_bytes())
    sidecar_package = sidecar_package or package
    tables = {name: (sidecar_package / file).read_bytes() for name, file in (sidecars or {}).items()}
    extra_pages, draw_remap = {}, None
    if scene == MATO_SCENE:
        if tables or switches is not None:
            raise ValueError("Unexpected indexed sidecars in Mato's room")
        extra_pages = mato_rest(table, research.parents[2] if research is not None else source_root())
    if scene == "Fungus2_15":
        if tables:
            raise ValueError("Unexpected draw-indexed sidecars in the Mantis arena")
        root = research or source_root() / "_private/research/crossroads-scenes"
        extra_pages, draw_remap = mantis_arena(Image, table, root / scene / "visuals/level180")
    swaps, transforms = sprite_swaps(tables, len(table.sprites), table.draws)
    switch_uses = list(switch_sprite_uses(switches, table, draw_remap)) if switches is not None else []
    for _, sprite, draw in switch_uses:
        swaps.setdefault(sprite, set()).add(draw)
    factors = density_factors(table, focal, swaps, max(1.0, scale_max), scaled, transforms)
    mapping, tail, classification = blur_plan(table, swaps, transforms)
    white_wave = white_wave_owners(research, scene, (owner for _, owner, _ in table.draws))
    hidden_draws = [i for i, (_, owner, _) in enumerate(table.draws) if owner in white_wave]
    tail = hide_draws(tail, hidden_draws)
    duplicates = sum(s != target for s, target in mapping.items())
    converted_sidecars = baked_sidecars(tables, table, mapping, len(table.sprites)+duplicates, blur_side)
    blurred_factors = behind_factors(table, focal, swaps, transforms, max(1., scale_max), scaled)
    for _, sprite, draw in switch_uses:
        _, owner, values = table.draws[draw]
        if blur_side(table.sprites[sprite], values, table.split_depth) == "behind":
            factor = draw_factor(table.sprites[sprite], values, focal, max(1., scale_max) if owner in scaled else 1.)
            previous = blurred_factors[sprite]
            if factor is not None:
                blurred_factors[sprite] = factor if previous is None else tuple(map(max, zip(previous, factor)))
    sources = dict(extra_pages)
    baked, blur_stats = {}, {}
    for original, target in mapping.items():
        sprite = table.sprites[original]
        page, x, y, w, h = sprite[:5]
        if page not in sources:
            sources[page] = Image.open(package / page_pattern.format(n=page)).convert("RGBA")
        factor = blurred_factors[original]
        if factor is None or min(factor) <= 0:
            raise ValueError(f"behind sprite {original} has no positive density")
        baked[target] = bake_blur(Image, sources[page].crop((x, y, x+w, y+h)), sprite, factor)
        blur_stats[str(original)] = dict(baked[target][2], target=target, density_factor=list(factor))
    archive_bank = None
    if scene == archive_particles.SCENE:
        first_sprite = len(table.sprites) + duplicates
        source = research.parents[2] if research is not None else source_root()
        particles, archive_bank = archive_particles.compile_archive(source, first_sprite)
        gc.collect()
        for index, image in enumerate(particles, first_sprite):
            baked[index] = (image, (-.5, -.5, .5, .5), {})
    tiles = []
    # A room sprite no draw, swap, switch or transform uses is never on screen. Keep its index but pack it
    # as one texel: player builds export extra unused sprites (Crossroads_10: 58 of 233, 5 -> 16 pages).
    packed_factors = [(0.0, 0.0) if f is None else f for f in factors]
    sizes, page_count, page_stats, sprite_bytes = build_pages(Image, package, page_pattern, table.pages,
                                                              table.sprites, packed_factors, out, baked, tiles,
                                                              extra_pages=extra_pages)
    version = 2 if tiles else PSR_VERSION
    header = b"PSR1" + struct.pack("<6I", version, PAGE, page_count, len(sizes), len(table.draws),
                                   (len(table.tail) - 72 * len(table.draws) - (4 if table.magic == b"HKR4" else 0)) // 24)
    tiled_tail = struct.pack("<I", len(tiles)) if tiles else b""
    for index, parts in tiles:
        tiled_tail += struct.pack("<2I", index, len(parts))
        for part in parts:
            tiled_tail += struct.pack("<7I4f", *part)
    (out / "room-visual.psp.bin").write_bytes(header + bytes(sprite_bytes) + tail + tiled_tail)
    switch_output = out / "room-visual.psp.bin.switches"
    if switches is not None:
        for offset, sprite, draw in switch_uses:
            if sprite in mapping and blur_side(table.sprites[sprite], table.draws[draw][2], table.split_depth) == "behind":
                struct.pack_into("<I", switches, offset, mapping[sprite])
        switch_output.write_bytes(switches)
    else:
        switch_output.unlink(missing_ok=True)
    for name, data in converted_sidecars.items():
        (out / sidecars[name]).write_bytes(data)
    if archive_bank is not None:
        (out / archive_particles.BANK).write_bytes(archive_bank)
    output_factors = list(factors)
    for original, target in mapping.items():
        if target == len(output_factors):
            output_factors.append(blurred_factors[original])
        else:
            output_factors[target] = blurred_factors[original]

    source_texels = sum(s[3] * s[4] for s in table.sprites)
    psp_texels = sum(w * h for w, h in sizes)
    manifest = {
        "scene": scene, "format": "PSR1", "version": version,
        "tiled_sprites": len(tiles), "tile_quads": sum(len(parts) for _, parts in tiles),
        "quantizer": "weighted-median-cut-premultiplied-srgb-alpha-no-dither",
        "source_table": table.magic.decode(), "focal_px": round(focal, 3),
        "page_size": PAGE, "pages": page_count, "texture_bytes": page_count * (PAGE * PAGE + 1024),
        "sprites": len(sizes), "draws": len(table.draws),
        "blur": {"draws": classification, "sprites": blur_stats,
                 "duplicates": duplicates,
                 "spacing_psp": BLUR_PSP_SPACING, "vibrance": BLUR_VIBRANCE,
                 "converted_sidecars": [sidecars[n] for n in converted_sidecars],
                 "sharp_pixels": "baseline palettes and packing retained"},
        "white_wave_owners": sorted(white_wave), "white_wave_draws": hidden_draws,
        "undrawn_sprites": [i for i, f in enumerate(factors) if f is None],
        "swapped_sprites": len(set(swaps) | set(transforms)), "sidecars": sorted(tables),
        "source_texels": source_texels, "psp_texels": psp_texels,
        "texel_ratio": round(psp_texels / source_texels, 4),
        "page_psnr": page_stats,
        "factors": [None if f is None else [round(min(1, f[0]), 4), round(min(1, f[1]), 4)] for f in output_factors],
        "source_sha256": {visual: sha256(package / visual),
                          **({switch_path.name: sha256(switch_path)} if switches is not None else {}),
                          **{filename: sha256(sidecar_package / filename) for filename in (sidecars or {}).values()},
                          **{page_pattern.format(n=n): sha256(package / page_pattern.format(n=n)) for n in range(table.pages)}},
        "not_done": ["particle sprites",
                     "colour-similarity page grouping", "static vertex buffers"] +
                    (["runtime selection of converted room sidecars"] if duplicates and tables else []),
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=1), encoding="utf-8")
    return manifest


ATLASES = {
    # name: (package folder under _private/converted, HKA1 table, page pattern)
    "hero": ("hero", "hero.bin", "page-{n}.png"),
}

PACKAGES = {
    # scene: (package folder under _private/converted, table, page pattern)
    "Tutorial_01": ("tutorial-static", "static-room-visual.bin", "static-room-page-{n}.png",
                    {"plants": "static-plants.bin", "dynamics": "static-room-dynamics.bin",
                     "ambient": "static-room-ambient.bin", "grass": "static-grass.bin"}),
}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("scene", choices=sorted(PACKAGES) + sorted(ATLASES))
    parser.add_argument("--out", type=Path, help="default: _private/converted-psp/<scene>")
    args = parser.parse_args(argv)
    if args.scene in ATLASES:
        folder, table_name, pattern = ATLASES[args.scene]
        out = args.out or REPO / "_private/converted-psp" / args.scene
        # Rebuild the hero from read-only original exports. The external converted
        # bank may predate required source clips (Door Control's Enter animation).
        # Both intermediate and PSP output belong to this checkout.
        import_pil()
        sys.path.insert(0, str(REPO / "tools"))
        from convert_hero import convert
        from types import SimpleNamespace
        package = REPO / "_private/converted-psp/hero-source"
        convert(SimpleNamespace(assets=source_root() / "_private/research/assets",
                                output=package, all=False,
                                tex3ds=None))
        m = transcode_atlas(package, table_name, pattern, out, args.scene)
        print(f"{args.scene}: {m['sprites']} sprites, {m['pages']} pages x {PAGE}, {m['texture_bytes']} bytes, "
              f"texels {m['texel_ratio']} of source, source-size {len(m['source_size_sprites'])}, "
              f"PSNR min {min(p['psnr_db'] for p in m['page_psnr'])} dB -> {out}")
        return 0
    folder, visual, pattern, sidecars = PACKAGES[args.scene]
    package = source_root() / "_private/converted" / folder
    out = args.out or REPO / "_private/converted-psp" / args.scene
    manifest = transcode(package, visual, pattern, out, args.scene, sidecars)
    psnr = [p["psnr_db"] for p in manifest["page_psnr"]]
    print(f"{args.scene}: {manifest['sprites']} sprites, {manifest['pages']} pages x {PAGE}, "
          f"{manifest['texture_bytes']} bytes, texels {manifest['texel_ratio']} of source, "
          f"undrawn {len(manifest['undrawn_sprites'])}, PSNR min {min(psnr)} dB -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
