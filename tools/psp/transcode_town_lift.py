"""Town's original lever/cage atlas; run under validation-lock.ps1 -Write."""
import argparse
import json
import math
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import transcode_room as room

HEADER = struct.Struct('<4s32s64s4II8II64I11f2I')


def sample_native(Image, crop, size):
    """Bilinear centre fetches in encoded, straight RGBA, without a minify filter.

    Resizing with BOX averages a source footprint, and doing so in linear light
    brightens thin dark outlines. Match this source collection's bilinear,
    straight-alpha fetches. F channels avoid Pillow's implicit premultiplication
    and keep rounding until after the four-channel fetch.
    """
    if crop.size == size:
        return crop
    matrix = (crop.width / size[0], 0, 0, 0, crop.height / size[1], 0)
    channels = []
    for channel in crop.split():
        values = channel.convert('F').transform(size, Image.Transform.AFFINE, matrix,
                                                Image.Resampling.BILINEAR).getdata()
        channels.append(Image.frombytes('L', size, bytes(round(v) for v in values)))
    return Image.merge('RGBA', channels)


def transforms(raw, table):
    if len(raw) != HEADER.size + 2 * 68 + 12 or raw[:4] != b'TLR2':
        raise ValueError('Town lift room size/magic')
    header = HEADER.unpack_from(raw)
    if header[1].split(b'\0', 1)[0] != b'Town':
        raise ValueError('Town lift scene')
    poses = [struct.unpack_from('<I16f', raw, HEADER.size + i * 68)[1:] for i in range(2)]
    if any(not all(math.isfinite(v) for v in p) or p[13] or p[14] or 38.1 + p[6] <= 0 for p in poses):
        raise ValueError('Unsupported Town lift transform')
    groups = [set(), set()]
    groups[0].add(header[-2]); groups[1].add(header[-1])
    for name, frames in table.clips.items():
        if name == b'Cage Open':
            groups[1].update(frames)
        elif name.startswith(b'Lever '):
            groups[0].update(frames)
        else:
            raise ValueError('Unknown Town lift clip ' + repr(name))
    if set.union(*groups) != set(range(len(table.sprites))):
        raise ValueError('Town lift sprite coverage')
    result = []
    for sprite in range(len(table.sprites)):
        # The two source draws own different animation libraries/transforms.
        selected = [p for p, ids in zip(poses, groups) if sprite in ids]
        result.append((max(math.hypot(p[0], p[3]) / (38.1 + p[6]) for p in selected),
                       max(math.hypot(p[1], p[4]) / (38.1 + p[6]) for p in selected)))
    return result


def convert(source, output):
    package = source / '_private/converted/hero/romfs/scenes/Town'
    table = room.AnimationTable((package / 'town-lift.bin').read_bytes())
    scale = transforms((package / 'town-lift-room.bin').read_bytes(), table)
    focal = room.room_focal_length(480, 272)
    factors = [(min(1., (s[7]-s[5])*sx*focal/s[3]), min(1., (s[8]-s[6])*sy*focal/s[4]))
               for s, (sx, sy) in zip(table.sprites, scale)]
    out = output / 'town-lift'
    out.mkdir(parents=True, exist_ok=True)
    sizes, pages, stats, sprites = room.build_pages(room.import_pil(), package, 'town-lift-page-{n}.png',
                                                  table.pages, table.sprites, factors, out,
                                                  sampler=sample_native)
    data = (b'PSA1' + struct.pack('<6I', 1, 512, pages, len(sizes), len(table.clip_bytes)//68, len(table.frames)) +
            sprites + table.clip_bytes + struct.pack(f'<{len(table.frames)}I', *table.frames) + table.event_bytes)
    (out / 'town-lift.psp.bin').write_bytes(data)
    report = dict(sprites=len(sizes), clips=len(table.clip_bytes)//68, frames=len(table.frames), pages=pages,
                  table_bytes=len(data), texture_bytes=pages*(512*512+1040), factors=factors, page_stats=stats,
                  sampling='bilinear centre, encoded straight RGBA; no area prefilter',
                  source_sha256={p.name: room.sha256(p) for p in
                                 [package/'town-lift.bin', package/'town-lift-room.bin'] +
                                 [package/f'town-lift-page-{i}.png' for i in range(table.pages)]})
    (out / 'manifest.json').write_text(json.dumps(report, indent=1) + '\n')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=room.source_root())
    parser.add_argument('--out', type=Path, default=room.REPO/'_private/converted-psp')
    args = parser.parse_args()
    print(json.dumps(convert(args.source, args.out)))
