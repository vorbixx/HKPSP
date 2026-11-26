"""Transcode the stock Crossroads_06 pillar and its dust into PSP CLUT8 pages."""
import argparse
import json
import math
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import transcode_room as room
from transcode_town_lift import sample_native


def projection(table, poses, effects):
    pose = struct.Struct('<2I16f')
    emitter = struct.Struct('<6I89f')
    if len(poses) != 44 + 2 * pose.size or poses[:4] != b'SHP1':
        raise ValueError('Shaman pillar room size/magic')
    if len(effects) != 8 + 6 * emitter.size or effects[:8] != struct.pack('<4sI', b'SPF1', 6):
        raise ValueError('Shaman pillar dust size/magic')
    scales = [None] * len(table.sprites)

    def record(indices, depth, horizontal, vertical):
        if (not all(map(math.isfinite, (depth, horizontal, vertical))) or depth <= -38.1 or
                min(horizontal, vertical) <= 0):
            raise ValueError('Unsupported shaman pillar projection')
        for index in indices:
            if not 0 <= index < len(scales):
                raise ValueError('Shaman pillar sprite outside atlas')
            previous = scales[index] or (0, 0)
            scales[index] = tuple(max(old, new / (38.1 + depth))
                                  for old, new in zip(previous, (horizontal, vertical)))

    for slot, names in enumerate(((b'Switch Idle', b'Switch Hit', b'Switch Activated'),
                                  (b'Pillar Shake', b'Pillar Open', b'Pillar Opened'))):
        owner, sprite, *values = pose.unpack_from(poses, 44 + slot * pose.size)
        if owner != (512, 120)[slot] or values[13] or values[14]:
            raise ValueError('Unsupported shaman pillar pose')
        indices = {sprite}
        for name in names:
            indices.update(table.clips[name])
        record(indices, values[6], math.hypot(values[0], values[3]), math.hypot(values[1], values[4]))

    for offset in range(8, len(effects), emitter.size):
        owner, _, first, count, *tail = emitter.unpack_from(effects, offset)
        values = tail[2:]
        if tuple(range(first, first + count)) != table.clips[f'Pillar dust {owner}'.encode()]:
            raise ValueError('Shaman pillar dust sprite order')
        size = values[7] * max(values[50:66])
        record(range(first, first + count), values[86], size, size)

    if None in scales:
        raise ValueError('Shaman pillar sprite coverage')
    return scales


def convert(source, output):
    package = source / '_private/converted/hero/romfs/scenes/Crossroads_06'
    prefix = 'shaman-pillar'
    inputs = [package / (prefix + suffix) for suffix in ('.bin', '-room.bin', '-fx.bin')]
    table = room.AnimationTable(inputs[0].read_bytes())
    scales = projection(table, inputs[1].read_bytes(), inputs[2].read_bytes())
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
