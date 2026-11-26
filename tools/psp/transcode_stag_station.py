"""Transcode the Crossroads_47 stag station art into PSP CLUT8 pages.

The 3DS bank (stag-station-art.bin, HKA1) and its four draws (stag-station-art-room.bin, SSA1) are the
source; the output is a PSA1 table with the same sprite and clip order, plus page-N.hpt files.
Each sprite is scaled to the largest on-screen size of the draws that can show it (the StagToll
owners 206 Box, 209 Grate, 205 Bell, 17 Front; clip names select the sprites of each draw).
The projection follows transcode_waterways.py: horizontal/vertical scale from the draw matrix
(values 0,3 and 1,4) and depth at values[6].
"""
import argparse
import json
import math
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import transcode_room as room
from transcode_town_lift import sample_native

# Same layout as HK3DS include/world_stag_toll.hpp loadArt: SSA1, count 4, then (sprite, owner, values[16]).
ROOM_HEADER = struct.Struct('<4sI')
DRAW = struct.Struct('<2I16f')
OWNERS = (206, 209, 205, 17)
# bindArt names every clip the station plays, grouped by the draw that shows it.
GROUPS = ((b'Box_idle', b'Box_disappear'),
          (b'Grate_idle', b'Grate_disappear'),
          (b'Bell_idle', b'Bell_stand_appear', b'Bell_stand'),
          (b'Front',))


def projection(table, room_bytes):
    if len(room_bytes) != ROOM_HEADER.size + 4 * DRAW.size or room_bytes[:4] != b'SSA1':
        raise ValueError('Stag station room size/magic')
    if struct.unpack_from('<I', room_bytes, 4)[0] != 4:
        raise ValueError('Stag station draw count')
    scales = [None] * len(table.sprites)
    for slot in range(4):
        sprite, owner, *values = DRAW.unpack_from(room_bytes, ROOM_HEADER.size + slot * DRAW.size)
        if owner != OWNERS[slot] or values[13] or values[14]:
            raise ValueError('Unsupported stag station draw')
        if not 0 <= sprite < len(table.sprites):
            raise ValueError('Stag station sprite outside atlas')
        depth = values[6]
        horizontal = math.hypot(values[0], values[3])
        vertical = math.hypot(values[1], values[4])
        if (not all(map(math.isfinite, (depth, horizontal, vertical))) or depth <= -38.1 or
                min(horizontal, vertical) <= 0):
            raise ValueError('Unsupported stag station projection')
        indices = {sprite}
        for name in GROUPS[slot]:
            indices.update(table.clips[name])
        for index in indices:
            previous = scales[index] or (0, 0)
            scales[index] = tuple(max(old, new / (38.1 + depth))
                                  for old, new in zip(previous, (horizontal, vertical)))
    # Sprites no draw can show keep an unscaled factor; they are never drawn.
    unused = [index for index, scale in enumerate(scales) if scale is None]
    return [scale or (1.0, 1.0) for scale in scales], unused


def convert(source, output):
    package = source / '_private/converted/hero/romfs/scenes/Crossroads_47'
    prefix = 'stag-station-art'
    inputs = [package / (prefix + '.bin'), package / (prefix + '-room.bin')]
    table = room.AnimationTable(inputs[0].read_bytes())
    for name in (b'Box_idle', b'Box_disappear', b'Grate_idle', b'Grate_disappear', b'Bell_idle',
                 b'Bell_stand_appear', b'Bell_stand', b'Front'):
        if name not in table.clips:
            raise ValueError(f'Stag station clip missing: {name.decode()}')
    scales, unused = projection(table, inputs[1].read_bytes())
    focal = room.room_focal_length(480, 272)
    factors = [(min(1., (sprite[7] - sprite[5]) * sx * focal / sprite[3]),
                min(1., (sprite[8] - sprite[6]) * sy * focal / sprite[4]))
               for sprite, (sx, sy) in zip(table.sprites, scales)]
    out = output / prefix
    out.mkdir(parents=True, exist_ok=True)
    sizes, pages, stats, sprites = room.build_pages(room.import_pil(), package, prefix + '-page-{n}.png',
                                                   table.pages, table.sprites, factors, out,
                                                   sampler=sample_native)
    data = (b'PSA1' + struct.pack('<6I', 1, 512, pages, len(sizes), table.n_clips, len(table.frames)) +
            sprites + table.clip_bytes + struct.pack(f'<{len(table.frames)}I', *table.frames) + table.event_bytes)
    (out / (prefix + '.psp.bin')).write_bytes(data)
    inputs.extend(package / f'{prefix}-page-{index}.png' for index in range(table.pages))
    report = dict(sprites=len(sizes), clips=table.n_clips, frames=len(table.frames), pages=pages,
                  table_bytes=len(data), texture_bytes=pages * (512 * 512 + 1040), factors=factors,
                  unused_sprites=unused, page_stats=stats,
                  source_sha256={path.name: room.sha256(path) for path in inputs})
    (out / 'manifest.json').write_text(json.dumps(report, indent=1) + '\n')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=room.source_root())
    parser.add_argument('--out', type=Path, default=room.REPO / '_private/converted-psp')
    args = parser.parse_args()
    result = convert(args.source, args.out)
    print(json.dumps({key: result[key] for key in ('sprites', 'clips', 'frames', 'pages', 'texture_bytes')}))
    print(json.dumps({'unused_sprites': result['unused_sprites']}))
