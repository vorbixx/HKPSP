"""Transcode the stock Crossroads_49b/49 city lift door pair and call switch into PSP CLUT8 pages."""
import argparse
import json
import math
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import transcode_room as room
from transcode_town_lift import sample_native

DRAW = struct.Struct('<2I16f')
SCENE = 'Crossroads_49b'


def transforms(raw, table):
    # CLA2: 'CLA2', two sprite indices (door, switch), then one 72-byte draw each: door Open/Close x2, call switch.
    if len(raw) != 12 + 3 * DRAW.size or raw[:4] != b'CLA2':
        raise ValueError('City lift room size/magic')
    draws = [DRAW.unpack_from(raw, 12 + i * DRAW.size) for i in range(3)]
    poses = [d[2:] for d in draws]
    if any(d[0] != 0 or not all(math.isfinite(v) for v in d[2:]) or 38.1 + d[8] <= 0 for d in draws):
        raise ValueError('Unsupported city lift transform')
    groups = [set(), set(), set()]
    for name, frames in table.clips.items():
        if name in (b'Lift Door Open', b'Lift Door Close'):
            groups[0].update(frames); groups[1].update(frames)
        elif name.startswith(b'Call Switch '):
            groups[2].update(frames)
        else:
            raise ValueError('Unknown city lift clip ' + repr(name))
    if set.union(*groups) != set(range(len(table.sprites))):
        raise ValueError('City lift sprite coverage')
    result = []
    for sprite in range(len(table.sprites)):
        selected = [p for p, ids in zip(poses, groups) if sprite in ids]
        if not selected:
            raise ValueError('City lift sprite without draw')
        result.append((max(math.hypot(p[0], p[3]) / (38.1 + p[6]) for p in selected),
                       max(math.hypot(p[1], p[4]) / (38.1 + p[6]) for p in selected)))
    return result


def convert(source, output):
    package = source / '_private/converted/hero/romfs/scenes' / SCENE
    table = room.AnimationTable((package / 'city-lift-art.bin').read_bytes())
    scale = transforms((package / 'city-lift-art-room.bin').read_bytes(), table)
    focal = room.room_focal_length(480, 272)
    factors = [(min(1., (s[7]-s[5])*sx*focal/s[3]), min(1., (s[8]-s[6])*sy*focal/s[4]))
               for s, (sx, sy) in zip(table.sprites, scale)]
    out = output / 'city-lift-art'
    out.mkdir(parents=True, exist_ok=True)
    sizes, pages, stats, sprites = room.build_pages(room.import_pil(), package, 'city-lift-art-page-{n}.png',
                                                  table.pages, table.sprites, factors, out,
                                                  sampler=sample_native)
    data = (b'PSA1' + struct.pack('<6I', 1, 512, pages, len(sizes), table.n_clips, len(table.frames)) +
            sprites + table.clip_bytes + struct.pack(f'<{len(table.frames)}I', *table.frames) + table.event_bytes)
    (out / 'city-lift-art.psp.bin').write_bytes(data)
    inputs = [package / n for n in ('city-lift-art.bin', 'city-lift-art-room.bin')]
    inputs += [package / f'city-lift-art-page-{i}.png' for i in range(table.pages)]
    report = dict(sprites=len(sizes), clips=table.n_clips, frames=len(table.frames), pages=pages,
                  table_bytes=len(data), texture_bytes=pages * (512 * 512 + 1040), factors=factors,
                  page_stats=stats, sampling='bilinear centre, encoded straight RGBA; no area prefilter',
                  source_sha256={p.name: room.sha256(p) for p in inputs})
    (out / 'manifest.json').write_text(json.dumps(report, indent=1) + '\n')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=room.source_root())
    parser.add_argument('--out', type=Path, default=room.REPO/'_private/converted-psp')
    args = parser.parse_args()
    print(json.dumps(convert(args.source, args.out)))
