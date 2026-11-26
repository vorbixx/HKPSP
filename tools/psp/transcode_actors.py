"""PSA1 actor atlases, retaining indices/geometry and bounding authored transforms.

Run under validation-lock.ps1 -Write. Reads HK3DS only. All pages use the same
median-cut quantizer as the room; a density factor is always <= 1 (no upscaling).
"""
import argparse
import json
import math
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import transcode_room as room
from actor_members import ATLASES


class Reader:
    def __init__(self, path, magic):
        self.data = path.read_bytes()
        if self.data[:4] != magic:
            raise ValueError(f'{path}: expected {magic!r}')
        self.pos = 4

    def read(self, fmt):
        result = struct.unpack_from('<'+fmt, self.data, self.pos)
        self.pos += struct.calcsize('<'+fmt)
        if any(isinstance(v, float) and not math.isfinite(v) for v in result):
            raise ValueError('Nonfinite actor transform')
        return result


def bounds(name, package, table):
    """Per-sprite max XY scale/depth. Indices and clip membership are untouched."""
    result = [(1., 1., 0.) for _ in table.sprites]

    def add(ids, sx, sy, z=0):
        for i in ids:
            if i >= len(result):
                raise ValueError('Actor sprite out of range')
            x, y, depth = result[i]
            result[i] = max(x, abs(sx)), max(y, abs(sy)), min(depth, z)

    def clip(key, sx, sy=None, z=0):
        key = key.encode()
        if key not in table.clips:
            raise ValueError(f'Missing actor clip {key!r}')
        add(table.clips[key], sx, sx if sy is None else sy, z)

    if name == 'great-door':
        # main.cpp:469-480: the atlas geometry already contains parent transforms.
        result = [(1., 1., .61) for _ in table.sprites]
    elif name == 'shade':
        # main.cpp:583-628, shade.cpp: hit quads / local child transforms.
        clip('Recovery Beam', 1.2682844)
        clip('Death Glow', 3)
        for key in table.clips:
            if b'Slash' in key or b'Hit' in key:
                add(table.clips[key], 3, 3)
    elif name == 'geo':
        r = Reader(package/'geo-rocks.bin', b'GRK1')
        count, = r.read('I')
        for _ in range(count):
            r.read('3I')
            v = r.read('9f')
            for key in r.read('48s48s48s'):
                clip(key.split(b'\0')[0].decode(), v[3], v[4], v[2])
        if r.read('4s')[0] != b'GCH2':
            raise ValueError('Geo chip config changed')
        r.read('24I')  # four profiles (integer counts followed by four floats)
        v = r.read('17f')
        clip('Rock Chips', v[3], z=v[5]-v[6])
        # HCC3: body/splat scales and depths; child cap/projectile scales are
        # source-limited at this resolution, retain their texels conservatively.
        r = Reader(package/'health-cocoon.bin', b'HCC3')
        r.read('I'); v = r.read('16f')
        add(range(len(result)), max(3, abs(v[3]), abs(v[4]), abs(v[13]), abs(v[14])),
            max(3, abs(v[3]), abs(v[4]), abs(v[13]), abs(v[14])), min(v[2], v[12]))
    elif name == 'fury':
        r = Reader(package/'fury-chest.bin', b'FCH3')
        v = r.read('61f'); r.read('I')
        # Shared clip bank also holds hit/tink effects with randomized scale <=3.
        sx = sy = 3.; depth = 0.
        for i in range(8):
            sx = max(sx, abs(v[i*5+3])); sy = max(sy, abs(v[i*5+4]))
            depth = min(depth, v[i*5+2])
        for _ in range(4):
            e = r.read('56f')
            size = max(abs(e[15]), abs(e[16]))*max(map(abs, e[24:56]))
            sx = max(sx, size); sy = max(sy, size); depth = min(depth, e[2])
        add(range(len(result)), sx, sy, depth)
    elif name == 'decor':
        r = Reader(package/'decor-effects.bin', b'HFX7')
        count, profiles, _, _ = r.read('4I')
        emitters = []
        for _ in range(count):
            sprite, tiles, _ = r.read('3I')
            v = r.read('60f'); r.read('42f')  # rate, cone, bases, axes
            emitters.append((sprite, tiles, v))
        # OWF2 provides per-axis particle scale for the same emitter indices.
        owners = Reader(package/'owner-effects.bin', b'OWF2')
        n, = owners.read('I'); owners.read(f'{n*8}I')
        n, = owners.read('I'); scales = {}
        for _ in range(n):
            emitter, _, _ = owners.read('3I'); owners.read('2f')
            scales[emitter] = owners.read('2f'); owners.read('2f')
        for i, (sprite, tiles, v) in enumerate(emitters):
            size = max(abs(v[11]), abs(v[12]))*max(map(abs, v[22:30]))
            sx, sy = scales.get(i, (1, 1))
            add(range(sprite, sprite+tiles), size*sx, size*sy, v[2])
        # Flash records have their own draw templates, rather than emitter sizes.
        for i, _, v in table.draws:
            add([i], math.hypot(v[0], v[3]), math.hypot(v[1], v[4]), v[6])
        # Great Door / initial fall / stalactite dynamic flashes: their source
        # animation templates and runtime random nail scale (max16) bound them.
        r.read(f'{profiles*9}I')
        for _ in range(2):
            sprite, count, _ = r.read('2If')
            add(range(sprite, sprite+count), 16, 16)
        for sidecar, magic, offset in (('stalactite-fx.bin', b'STF1', 84),
                                        ('initial-fall-fx.bin', b'IFX1', 104)):
            f = Reader(package/sidecar, magic)
            f.pos = offset
            sprite, count, _ = f.read('2If')
            add(range(sprite, sprite+count), 1, 1)
        # STV1 hanging/embedded templates are in this same atlas.
        r = Reader(package/'stalactite-visuals.bin', b'STV1')
        n, = r.read('I')
        for _ in range(n):
            r.read('I')
            for _ in range(2):
                sprite, = r.read('I'); v = r.read('16f')
                add([sprite], math.hypot(v[0], v[3]), math.hypot(v[1], v[4]), v[6])
    return result


def convert(source, output):
    package = source/'_private/converted/hero/romfs'
    Image = room.import_pil()
    summaries = {}
    for filename, (name, _) in ATLASES.items():
        if name == 'hero':
            continue
        raw = (package/filename).read_bytes()
        if name == 'decor':
            # HKR2 has the same sprite/draw records as HKR3, with no split tail.
            if raw[:4] != b'HKR2':
                raise ValueError('Expected decor HKR2')
            table = room.RoomTable(b'HKR3'+raw[4:])
            # Animation wrapper retains every original sprite index.
            table.clips = {}
            clips = struct.pack('<48sf4I', b'Decor', 1., 0, 0, 0, len(table.sprites))
            frames = tuple(range(len(table.sprites)))
        else:
            table = room.AnimationTable(raw)
            clips, frames = table.clip_bytes, table.frames
        transforms = bounds(name, package, table)
        factors = []
        for s, (sx, sy, z) in zip(table.sprites, transforms):
            ppu = room.room_focal_length(480, 272)/(38.1+z)
            if ppu <= 0:
                raise ValueError('Actor crosses camera plane')
            factors.append((min(1., (s[7]-s[5])*sx*ppu/s[3]),
                            min(1., (s[8]-s[6])*sy*ppu/s[4])))
        out = output/name
        prefix = 'great-door' if name == 'great-door' else 'decor-fx' if name == 'decor' else name
        art_folder = {'geo': 'geo-visuals', 'great-door': 'great-door-visuals',
                      'fury': 'fury-chest', 'decor': 'decor-effects',
                      'events': 'conditional-events'}.get(name, name)
        sizes, pages, stats, sprites = room.build_pages(Image, source/'_private/converted'/art_folder, prefix+'-page-{n}.png',
            table.pages, table.sprites, factors, out)
        data = b'PSA1'+struct.pack('<6I', 1, 512, pages, len(sizes), len(clips)//68, len(frames))
        data += sprites+clips+struct.pack(f'<{len(frames)}I', *frames)+getattr(table, 'event_bytes', b'')
        (out/filename.replace('.bin', '.psp.bin')).write_bytes(data)
        manifest = dict(table=filename, sprites=len(sizes), pages=pages, factors=factors,
                        transforms=transforms, page_stats=stats, source_sha256=room.sha256(package/filename),
                        texture_bytes=pages*(512*512+1040))
        (out/'manifest.json').write_text(json.dumps(manifest, indent=1)+'\n')
        summaries[name] = dict(sprites=len(sizes), pages=pages, texture_bytes=manifest['texture_bytes'])
    return summaries


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=room.source_root())
    parser.add_argument('--out', type=Path, default=room.REPO/'_private/converted-psp')
    args = parser.parse_args()
    print(json.dumps(convert(args.source, args.out)))
