"""Transcode the Ruins1_05b waterways machine art into PSP CLUT8 pages.

The 3DS bank (waterways-art.bin, HKA1) and its draws (waterways-machine.bin, WWM1)
are the source; the output is a PSA1 table with the same sprite and clip order.
Each sprite is scaled to the largest on-screen size of the draw slots that show it
(WaterwaysRoom::updateArt: slot 0 plays Machine Open, slot 1 Manhole Open, slots 2
and 3 show their idle sprite). Sprite-to-slot pairs are taken from the WWM1 header.
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

# Same layout as HK3DS tools/convert_waterways_machine.py FORMAT.
HEADER = struct.Struct('<4s6f64s64s7f4I768s768s32s32s32s4I')
DRAW = struct.Struct('<I16f')
SLOT_CLIPS = (b'Machine Open', b'Manhole Open', None, None)


def projection(table, machine):
    if len(machine) != HEADER.size + 4 * DRAW.size or machine[:4] != b'WWM1':
        raise ValueError('Waterways machine size/magic')
    sprites = HEADER.unpack_from(machine)[-4:]
    if any(not 0 <= index < len(table.sprites) for index in sprites):
        raise ValueError('Waterways idle sprite outside atlas')
    scales = [None] * len(table.sprites)
    for slot in range(4):
        owner, *values = DRAW.unpack_from(machine, HEADER.size + slot * DRAW.size)
        if owner == 0 or values[13] or values[14]:
            raise ValueError('Unsupported waterways draw')
        depth = values[6]
        horizontal = math.hypot(values[0], values[3])
        vertical = math.hypot(values[1], values[4])
        if (not all(map(math.isfinite, (depth, horizontal, vertical))) or depth <= -38.1 or
                min(horizontal, vertical) <= 0):
            raise ValueError('Unsupported waterways projection')
        indices = {sprites[slot]}
        if SLOT_CLIPS[slot]:
            indices.update(table.clips[SLOT_CLIPS[slot]])
        for index in indices:
            previous = scales[index] or (0, 0)
            scales[index] = tuple(max(old, new / (38.1 + depth))
                                  for old, new in zip(previous, (horizontal, vertical)))
    # Sprites no slot can show keep an unscaled factor; they are never drawn.
    unused = [index for index, scale in enumerate(scales) if scale is None]
    return [scale or (1.0, 1.0) for scale in scales], unused


def convert(source, output):
    package = source / '_private/converted/hero/romfs/scenes/Ruins1_05b'
    prefix = 'waterways-art'
    inputs = [package / (prefix + '.bin'), package / 'waterways-machine.bin']
    table = room.AnimationTable(inputs[0].read_bytes())
    if table.n_clips == 0 or b'Machine Open' not in table.clips or b'Manhole Open' not in table.clips:
        raise ValueError('Waterways clips missing')
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
