"""Transcode Fungus2_04's Mantis gate art into PSP CLUT8 pages; run under validation-lock.ps1 -Write.

Modelled on transcode_shaman_pillar.py. The 3DS romfs files (mantis-gate.bin, mantis-gate-room.bin,
mantis-gate-page-0.png) come from HK3DS convert_mantis_gates.py; the PSP table keeps the same sprite
order, so the shared MantisGates bank (3DS) can be remapped onto these pages at runtime.
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

SCENE = 'Fungus2_04'
PREFIX = 'mantis-gate'
# MTG1 layout (HK3DS include/world_mantis_runtime.hpp MantisGateData::load): magic, 3 waits, then per
# pair: u32 collider, two draws (u32 owner, u32 sprite, 16 floats), 10 floats of nail polygon.
ROOM_HEADER = struct.Struct('<4s3f')
DRAW = struct.Struct('<2I16f')
PAIR_OWNERS = ((143, 149), (215, 1157))
LEVER_CLIPS = (b'Lever Idle', b'Lever Hit', b'Lever Activated')
DOOR_CLIPS = (b'Door Closed', b'Door Open 1', b'Door Open 2')
PART_CLIPS = (LEVER_CLIPS, DOOR_CLIPS)
PAIR_SIZE = 4 + 2 * DRAW.size + 40
ROOM_SIZE = ROOM_HEADER.size + 2 * PAIR_SIZE


def parse_room(raw):
    """Return [[(sprite, values), (sprite, values)] per pair], checking owners and magic."""
    if len(raw) != ROOM_SIZE or raw[:4] != b'MTG1':
        raise ValueError('Mantis gate room size/magic')
    pairs = []
    offset = ROOM_HEADER.size
    for pair_owners in PAIR_OWNERS:
        offset += 4
        draws = []
        for owner_expected in pair_owners:
            owner, sprite, *values = DRAW.unpack_from(raw, offset)
            offset += DRAW.size
            if owner != owner_expected:
                raise ValueError('Unsupported Mantis gate owner')
            draws.append((sprite, values))
        offset += 40
        pairs.append(draws)
    return pairs


def projection(table, raw):
    pairs = parse_room(raw)
    scales = [None] * len(table.sprites)

    def record(indices, depth, horizontal, vertical):
        if (not all(map(math.isfinite, (depth, horizontal, vertical))) or depth <= -38.1 or
                min(horizontal, vertical) <= 0):
            raise ValueError('Unsupported Mantis gate projection')
        for index in indices:
            if not 0 <= index < len(scales):
                raise ValueError('Mantis gate sprite outside atlas')
            previous = scales[index] or (0, 0)
            scales[index] = tuple(max(old, new / (38.1 + depth))
                                  for old, new in zip(previous, (horizontal, vertical)))

    for draws in pairs:
        for (sprite, values), names in zip(draws, PART_CLIPS):
            if values[13] or values[14]:
                raise ValueError('Unsupported Mantis gate pose')
            indices = {sprite}
            for name in names:
                indices.update(table.clips[name])
            record(indices, values[6], math.hypot(values[0], values[3]), math.hypot(values[1], values[4]))

    if None in scales:
        raise ValueError('Mantis gate sprite coverage')
    return scales


def convert(source, output):
    package = source / '_private/converted/hero/romfs/scenes' / SCENE
    inputs = [package / (PREFIX + suffix) for suffix in ('.bin', '-room.bin')]
    table = room.AnimationTable(inputs[0].read_bytes())
    scales = projection(table, inputs[1].read_bytes())
    focal = room.room_focal_length(480, 272)
    factors = [(min(1., (sprite[7] - sprite[5]) * sx * focal / sprite[3]),
                min(1., (sprite[8] - sprite[6]) * sy * focal / sprite[4]))
               for sprite, (sx, sy) in zip(table.sprites, scales)]
    out = output / PREFIX
    out.mkdir(parents=True, exist_ok=True)
    sizes, pages, stats, sprites = room.build_pages(room.import_pil(), package, PREFIX + '-page-{n}.png',
                                                   table.pages, table.sprites, factors, out,
                                                   sampler=sample_native)
    data = (b'PSA1' + struct.pack('<6I', 1, 512, pages, len(sizes), table.n_clips, len(table.frames)) +
            sprites + table.clip_bytes + struct.pack(f'<{len(table.frames)}I', *table.frames) + table.event_bytes)
    (out / (PREFIX + '.psp.bin')).write_bytes(data)
    inputs.extend(package / f'{PREFIX}-page-{index}.png' for index in range(table.pages))
    report = dict(scene=SCENE, sprites=len(sizes), clips=table.n_clips, frames=len(table.frames), pages=pages,
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
