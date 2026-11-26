"""Build the PSP CLUT8 atlas for the City Crest dust, Slam and Bump rocks."""
import argparse
import math
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import transcode_room as room
from transcode_town_lift import sample_native


def convert(source, output):
    scene = source / '_private/converted/hero/romfs/scenes/Fungus2_21'
    prefix = 'city-crest-fx'
    table = room.AnimationTable((scene / f'{prefix}-art.bin').read_bytes())
    focal = room.room_focal_length(480, 272)
    factors = [None] * len(table.sprites)
    for name, magic, count, tail in (('fx', b'CCP1', 7, 0), ('rocks', b'CCR1', 1, 24)):
        raw = (scene / f'city-crest-{name}.bin').read_bytes()
        emitter = struct.Struct('<6I89f')
        if raw[:8] != struct.pack('<4sI', magic, count) or len(raw) != 8 + count * emitter.size + tail:
            raise ValueError(f'Invalid City Crest {name} emitters')
        for index in range(count):
            _, _, first, frames, _, _, *values = emitter.unpack_from(raw, 8 + index * emitter.size)
            scale = values[7] * max(values[50:66])
            for sprite in range(first, first + frames):
                factors[sprite] = room.draw_factor(table.sprites[sprite], (scale, 0, 0, 0, scale, 0, values[86]), focal)
    magic, first, _, *values = struct.unpack('<4s2I16f', (scene / 'city-crest-slam.bin').read_bytes())
    slam = table.clips[b'Slam']
    if magic != b'CCS1' or first != slam[0]:
        raise ValueError('Invalid City Crest Slam draw')
    for sprite in slam:
        factors[sprite] = room.draw_factor(table.sprites[sprite], values, focal)
    if any(factor is None or not all(math.isfinite(value) and value > 0 for value in factor) for factor in factors):
        raise ValueError('City Crest sprite missing projected density')
    out = output / prefix
    sizes, pages, _, sprites = room.build_pages(room.import_pil(), scene, prefix + '-page-{n}.png',
                                               table.pages, table.sprites, factors, out, sampler=sample_native)
    if pages != 1:
        raise ValueError(f'City Crest atlas exceeds one page: {pages}')
    data = (b'PSA1' + struct.pack('<6I', 1, 512, pages, len(sizes), table.n_clips, len(table.frames)) +
            sprites + table.clip_bytes + struct.pack(f'<{len(table.frames)}I', *table.frames) + table.event_bytes)
    (out / f'{prefix}.psp.bin').write_bytes(data)
    print(f'City Crest: {len(sizes)} sprites, {pages} CLUT8 page')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=room.source_root())
    parser.add_argument('--out', type=Path, default=room.REPO / '_private/converted-psp')
    args = parser.parse_args()
    convert(args.source, args.out)
