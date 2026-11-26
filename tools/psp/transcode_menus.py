"""Convert the original menu backdrop and title art to PSP CLUT8.

Run under tools/validation-lock.ps1 -Write. Inputs remain read-only.
"""
import argparse
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import transcode_room as room


WIDGETS = (
    ('heading', 2086, 'Warning_Fleur0008', (24, 0, 184, 16)),
    ('profile-frame', 1943, 'profile_fleur0012', (0, 28, 254, 27)),
    ('profile-selection', 3283, 'Profile_Selector', (0, 55, 254, 35)),
    ('pause-top', 3137, 'pause_top_fleur0008', (0, 92, 107, 31)),
    ('pause-bottom', 1697, 'bottom_fleur0008', (112, 92, 76, 17)),
)
SLIDER = ('audio-handle', 140, 'MenuSliderHandle', (192, 92, 15, 32))


def original_widgets(source):
    sys.path.insert(0, str(source / '_private/tools/boss-python'))
    try:
        import UnityPy
    finally:
        sys.path.pop(0)
    data = source / '_private/original/Hollow Knight/hollow_knight_Data'
    for archive, entries in (('resources.assets', WIDGETS), ('sharedassets0.assets', (SLIDER,))):
        objects = {obj.path_id: obj for obj in UnityPy.load(str(data / archive)).objects
                   if obj.type.name == 'Sprite'}
        for key, path_id, expected, bounds in entries:
            sprite = objects[path_id].read()
            if sprite.m_Name != expected:
                raise ValueError(f'{archive}:{path_id}: expected {expected}, got {sprite.m_Name}')
            yield key, bounds, sprite.image.convert('RGBA'), dict(
                collection=archive, path_id=path_id, sprite=expected)


def convert(source, output):
    package = source / '_private/converted/title-menu'
    backdrop = source / '_private/research/title-menu/menu-background-source.png'
    Image = room.import_pil()
    output.mkdir(parents=True, exist_ok=True)
    font_path = package / 'title-font.png'
    font = Image.open(font_path).convert('RGBA')
    if font.size != (512, 128):
        raise ValueError('title-font: expected a 512x128 title page')
    pages = {}
    for name, original, source_size, size, logical, rect in (
            ('title-background', backdrop, (1920, 1080), (512, 512), (480, 272), None),
            ('title-logo', package / 'title-logo.png', (512, 128), (512, 128), (300, 123), None),
            ('title-mote', font_path, (512, 128), (32, 32), (24, 32), (36, 96, 60, 128))):
        image = font if original == font_path else Image.open(original).convert('RGBA')
        if image.size != source_size:
            raise ValueError(f'{original.name}: expected source size {source_size}')
        if original == backdrop:
            page = Image.new('RGBA', size)
            page.paste(image.resize(logical, Image.Resampling.LANCZOS), (0, 0))
            image = page
        elif rect is not None:
            page = Image.new('RGBA', size)
            page.paste(image.crop(rect), (0, 0))
            image = page
        quant = room.quantize_page(Image, image)
        palette = bytes(quant.getpalette(rawmode='RGBA'))
        destination = output / (name + '.hpt')
        destination.write_bytes(room.hpt1(*size, palette, quant.tobytes()))
        pages[name] = dict(size=list(size), logical=list(logical),
                           source_sha256=room.sha256(original),
                           output_sha256=room.sha256(destination),
                           bytes=destination.stat().st_size)
    widgets = Image.new('RGBA', (256, 128))
    widgets.paste((255, 255, 255, 255), (0, 0, 16, 8))
    widgets.paste(font.crop((0, 104, 16, 120)), (0, 9))
    ornament_manifest = {}
    for key, (left, top, width, height), original, provenance in original_widgets(source):
        widgets.paste(original.resize((width, height), Image.Resampling.LANCZOS), (left, top))
        ornament_manifest[key] = dict(rect=[left, top, width, height], **provenance)
    quant = room.quantize_page(Image, widgets)
    fill = output / 'menu-fill.hpt'
    fill.write_bytes(room.hpt1(*widgets.size, bytes(quant.getpalette(rawmode='RGBA')), quant.tobytes()))
    manifest = dict(pages=pages, fill_bytes=fill.stat().st_size,
                    selection_arrow=dict(rect=[0, 9, 16, 25], source_sha256=room.sha256(font_path)),
                    ornaments=ornament_manifest,
                    source_manifest_sha256=room.sha256(package / 'title-menu-manifest.json'),
                    backdrop='Menu_Title backdrop with the logo and interface drawn separately.')
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=room.source_root())
    parser.add_argument('--out', type=Path, default=room.REPO / '_private/converted-psp/menus')
    args = parser.parse_args()
    print(json.dumps(convert(args.source, args.out), indent=2))
