"""Transcode the Crossroads_03 world switch art into PSP CLUT8 pages (world-actor path).

Same pipeline as transcode_shaman_pillar.py. The sprite list keeps HK3DS order, so the
switch and gate frame indices resolve unchanged; only the page table and pixels are PSP.
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

SCENE = 'Crossroads_03'
ROOM = struct.Struct('<I16f')
ROOM_HEADER = 120
# WorldSwitchVisuals parts: part 0 is the switch, parts 1 and 2 the two gates.
PART_CLIPS = ((b'Switch Idle', b'Switch Hit'), (b'Toll Gate Idle', b'Toll Gate Open'), (b'Toll Gate Idle', b'Toll Gate Open'))


def scales_for(table, room_bytes):
    if len(room_bytes) != ROOM_HEADER + 3 * ROOM.size or room_bytes[:4] != b'WSR1':
        raise ValueError('Crossroads_03 world switch room size/magic')
    idle = struct.unpack_from('<3I', room_bytes, 104)
    scales = [None] * len(table.sprites)
    for part in range(3):
        owner, *values = ROOM.unpack_from(room_bytes, ROOM_HEADER + part * ROOM.size)
        depth = values[6]
        if not math.isfinite(depth) or depth <= -38.1 or not all(math.isfinite(v) for v in values):
            raise ValueError('Unsupported world switch projection')
        horizontal, vertical = math.hypot(values[0], values[3]), math.hypot(values[1], values[4])
        if min(horizontal, vertical) <= 0:
            raise ValueError('Degenerate world switch projection')
        indices = {idle[part]} if idle[part] < len(table.sprites) else set()
        for name in PART_CLIPS[part]:
            indices.update(table.clips[name])
        for index in indices:
            previous = scales[index] or (0, 0)
            scales[index] = tuple(max(old, new / (38.1 + depth))
                                  for old, new in zip(previous, (horizontal, vertical)))
    return scales


def convert(source, output):
    package = source / '_private/converted/hero/romfs/scenes' / SCENE
    prefix = 'world-switch'
    inputs = [package / (prefix + suffix) for suffix in ('.bin', '-room.bin')]
    table = room.AnimationTable(inputs[0].read_bytes())
    scales = scales_for(table, inputs[1].read_bytes())
    focal = room.room_focal_length(480, 272)
    factors = [(1., 1.) if scale is None else
               (min(1., (sprite[7] - sprite[5]) * scale[0] * focal / sprite[3]),
                min(1., (sprite[8] - sprite[6]) * scale[1] * focal / sprite[4]))
               for sprite, scale in zip(table.sprites, scales)]
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
                  page_stats=stats, source_sha256={path.name: room.sha256(path) for path in inputs})
    (out / 'manifest.json').write_text(json.dumps(report, indent=1) + '\n')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=room.source_root())
    parser.add_argument('--out', type=Path, default=room.REPO / '_private/converted-psp')
    args = parser.parse_args()
    result = convert(args.source, args.out)
    print(json.dumps({key: result[key] for key in ('sprites', 'clips', 'frames', 'pages', 'texture_bytes')}))
