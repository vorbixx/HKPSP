"""Convert the frozen 3DS Cornifer bank to a room-local PSP CLUT8 atlas."""
import argparse
import json
from pathlib import Path
import re
import struct
import sys
from tempfile import TemporaryDirectory

sys.path.insert(0, str(Path(__file__).resolve().parent))
import transcode_room as room


def convert(source, output):
    package = source / '_private/converted/cornifer-original'
    source_table = package / 'cornifer-anim.bin'
    table = room.AnimationTable(source_table.read_bytes())
    if set(table.clips) != {b'Idle', b'Talk L', b'Talk R', b'Card'}:
        raise ValueError('Unexpected Cornifer clips')
    data = (room.REPO / 'include/cornifer_data.hpp').read_text()
    scales = [abs(float(re.search(rf'\bscale{axis}=(-?[\d.]+)f', data)[1])) for axis in 'XY']
    card = set(table.clips[b'Card'])
    actor = {sprite for name, frames in table.clips.items() if name != b'Card' for sprite in frames}
    ppu = room.room_focal_length(480, 272) / 38.1
    factors = []
    for index, sprite in enumerate(table.sprites):
        scale = [max(1. if index in card else 0., s if index in actor else 0.) for s in scales]
        factors.append((ppu*(sprite[7]-sprite[5])*scale[0]/sprite[3],
                        ppu*(sprite[8]-sprite[6])*scale[1]/sprite[4]))
    out = output / 'Crossroads_33'
    out.mkdir(parents=True, exist_ok=True)
    page_pattern = 'cornifer-page-{n}.png'
    with TemporaryDirectory(dir=out) as scratch:
        scratch = Path(scratch)
        sizes, pages, stats, sprites = room.build_pages(room.import_pil(), package, page_pattern,
                                                       table.pages, table.sprites, factors, scratch)
        if pages != 1:
            raise ValueError(f'Cornifer exceeds its one-page budget: {pages}')
        for suffix in ('hpt', 'png'):
            (scratch / f'page-0.{suffix}').replace(out / f'cornifer-page-0.{suffix}')
    data = (b'PSA1' + struct.pack('<6I', 1, 512, pages, len(sizes), table.n_clips, len(table.frames)) +
            sprites + table.clip_bytes + struct.pack(f'<{len(table.frames)}I', *table.frames) + table.event_bytes)
    (out / 'cornifer-visual.bin').write_bytes(data)
    paths = [source_table] + [package / page_pattern.format(n=i) for i in range(table.pages)]
    report = dict(pages=pages, clips=table.n_clips, sprites=len(sizes), frames=len(table.frames),
                  texture_bytes=pages*(512*512+1040), table_bytes=len(data), page_stats=stats,
                  source_sha256={p.name: room.sha256(p) for p in paths})
    (out / 'cornifer-manifest.json').write_text(json.dumps(report, indent=1) + '\n')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=room.source_root())
    parser.add_argument('--out', type=Path, default=room.REPO/'_private/converted-psp')
    args = parser.parse_args()
    print(json.dumps(convert(args.source, args.out)))
