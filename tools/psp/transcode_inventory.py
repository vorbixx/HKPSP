"""Read-only HK3DS inventory HKA1 -> PSP PSA1 / native-size CLUT8 art.

The source inventory sprites are already baked UI pixels, not world units.
Keep their geometry and density; the PSP layout never enlarges a sprite.
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


def source_shapes(Image):
    """Rasterize the native C2D ellipses in src/hud_renderer.cpp:199-210.

    No substitute charm art: these are the original empty slots and notches.
    """
    from PIL import ImageDraw
    shapes = []
    for name, size, color in [('NotchEmpty', 10, (53, 61, 79, 255)),
                              ('NotchUsed', 10, (232, 231, 220, 255)),
                              ('NotchOver', 10, (185, 83, 223, 255)),
                              ('CharmSlot', 20, (45, 49, 57, 255))]:
        image = Image.new('RGBA', (size * 4, size * 4))
        draw = ImageDraw.Draw(image)
        draw.ellipse((0, 0, size * 4 - 1, size * 4 - 1), fill=color)
        if name == 'CharmSlot':
            draw.ellipse((12, 12, 67, 67), fill=(5, 8, 13, 255))
        shapes.append((name, image.resize((size, size), Image.Resampling.LANCZOS)))
    return shapes


def convert(source, output):
    package = source / '_private/converted/inventory'
    table = room.AnimationTable((package / 'inventory.bin').read_bytes())
    Image = room.import_pil()
    output.mkdir(parents=True, exist_ok=True)
    source_sprites = list(table.sprites)
    clips, frames = table.clip_bytes, list(table.frames)
    shapes = source_shapes(Image)
    with tempfile.TemporaryDirectory(dir=output) as temporary:
        staging = Path(temporary)
        for n in range(table.pages):
            shutil.copyfile(package / f'inventory-page-{n}.png', staging / f'input-{n}.png')
        extra = Image.new('RGBA', (room.PAGE, room.PAGE))
        for n, (name, image) in enumerate(shapes):
            size = image.width
            extra.paste(image, (32 * n, 0))
            index = len(source_sprites)
            source_sprites.append((table.pages, 32 * n, 0, size, size,
                                   -size / 2, -size / 2, size / 2, size / 2))
            clips += struct.pack('<48sf4I', name.encode(), 1., 0, 0, len(frames), 1)
            frames.append(index)
        extra.save(staging / f'input-{table.pages}.png')
        sizes, pages, stats, sprites = room.build_pages(
            Image, staging, 'input-{n}.png', table.pages + 1, source_sprites,
            [(1., 1.)] * len(source_sprites), output)
    data = b'PSA1' + struct.pack('<6I', 1, room.PAGE, pages, len(sizes),
                               table.n_clips + len(shapes), len(frames))
    data += sprites + clips + struct.pack(f'<{len(frames)}I', *frames)
    data += table.event_bytes
    read_psa(data)
    (output / 'inventory.psp.bin').write_bytes(data)
    result = dict(format='PSA1', version=1, density='native UI pixels; draw scale <= 1',
                  pages=pages, sprites=len(sizes), clips=table.n_clips + len(shapes),
                  source_sprites=len(table.sprites), rasterized_source_primitives=[s[0] for s in shapes],
                  texture_bytes=pages * (room.PAGE * room.PAGE + 1040),
                  table_bytes=len(data), sizes=sizes, page_psnr=stats,
                  source_sha256={p.name: room.sha256(p) for p in
                      [package / 'inventory.bin', package / 'manifest.json',
                       *sorted(package.glob('inventory-page-*.png'))]})
    (output / 'manifest.json').write_text(json.dumps(result, indent=1) + '\n', encoding='utf-8')
    return result


if __name__ == '__main__':
    report = convert(room.source_root(), room.REPO / '_private/converted-psp/inventory')
    print(f"Inventory: {report['sprites']} sprites, {report['pages']} CLUT8 pages, "
          f"{report['texture_bytes']} texture bytes")
