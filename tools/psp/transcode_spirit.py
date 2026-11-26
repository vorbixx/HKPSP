"""Read-only HK3DS ability-pickups HKA1 -> PSP PSA1 Vengeful Spirit projectile bank.

Only the clips the 3DS draws for a cast spirit are kept (src/main.cpp:873-885 via
SpiritProjectile::animation, include/spells.hpp:22): "Spirit", "Spirit End" and
"Shade Soul". Their source is the original Fireball Ball / Ball End / Ball Lv2 clips
(tools/convert_ability_pickups.py:69-70). Sprites keep their source texels: at the PSP
room focal length the spirit draws at about 0.8-1.0 texel per screen pixel before the
1.2x or 1.8x spell scale, so no resampling is applied (texel <= screen pixel holds).
"""
import json
import struct
import sys
import shutil
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import transcode_room as room
from transcode_hud import read_psa

CLIPS = ('Spirit', 'Spirit End', 'Shade Soul')
SOURCE_PAGE_NAME = 'ability-page-0.png'


def convert(source, output):
    package = source / '_private/converted/ability-pickups'
    table = room.AnimationTable((package / 'ability-art.bin').read_bytes())
    if table.pages != 1 or table.event_bytes:
        raise ValueError('Unexpected ability-art layout (pages or events)')
    Image = room.import_pil()
    output.mkdir(parents=True, exist_ok=True)
    # Clip table rows: name, fps, wrap, loopStart, first frame, count (68 B each).
    rows = {}
    for offset in range(0, len(table.clip_bytes), 68):
        raw, fps, wrap, loop, first, count = struct.unpack_from('<48sf4I', table.clip_bytes, offset)
        rows.setdefault(raw.split(b'\0', 1)[0].decode(), (fps, wrap, loop, first, count))
    missing = [name for name in CLIPS if name not in rows]
    if missing:
        raise ValueError('Missing original spirit clips: ' + ', '.join(missing))
    # Compact sprite table: only sprites referenced by the kept clips, in first-use order.
    used, index = [], {}
    frames, clip_rows = [], b''
    for name in CLIPS:
        fps, wrap, loop, first, count = rows[name]
        offset = len(frames)
        for source_index in table.frames[first:first + count]:
            if source_index not in index:
                index[source_index] = len(used)
                used.append(source_index)
            frames.append(index[source_index])
        clip_rows += struct.pack('<48sf4I', name.encode(), fps, wrap, loop, offset, count)
    sprites = [table.sprites[i] for i in used]
    with tempfile.TemporaryDirectory(dir=output) as temporary:
        staging = Path(temporary)
        shutil.copyfile(package / SOURCE_PAGE_NAME, staging / 'input-0.png')
        factors = [(1., 1.)] * len(sprites)
        sizes, pages, stats, sprite_bytes = room.build_pages(
            Image, staging, 'input-{n}.png', 1,
            [(p, x, y, w, h, l, b, r, t) for (p, x, y, w, h, l, b, r, t) in sprites],
            factors, output)
    data = b'PSA1' + struct.pack('<6I', 1, room.PAGE, pages, len(sizes), len(CLIPS), len(frames))
    data += sprite_bytes + clip_rows + struct.pack(f'<{len(frames)}I', *frames)
    read_psa(data)
    (output / 'spirit.psp.bin').write_bytes(data)
    result = dict(format='PSA1', version=1, density='source texels; <= 1 texel per screen pixel',
                  clips=list(CLIPS), sprites=len(sizes), frames=len(frames), pages=pages,
                  texture_bytes=pages * (room.PAGE * room.PAGE + 1040), table_bytes=len(data),
                  sizes=sizes, page_psnr=stats,
                  source_sha256={p.name: room.sha256(p) for p in
                      [package / 'ability-art.bin', package / SOURCE_PAGE_NAME, package / 'manifest.json']})
    (output / 'spirit-manifest.json').write_text(json.dumps(result, indent=1) + '\n', encoding='utf-8')
    return result


if __name__ == '__main__':
    report = convert(room.source_root(), room.REPO / '_private/converted-psp/spirit')
    print(f"Spirit: {report['sprites']} sprites, {report['frames']} frames, {report['pages']} CLUT8 pages, "
          f"{report['texture_bytes']} texture bytes")
