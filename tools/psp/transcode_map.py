"""Convert the existing Crossroads map atlas to a PSP CLUT8 page."""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import transcode_room as room


def add_widgets(source, Image, page):
    sys.path.insert(0, str(source / '_private/tools/boss-python'))
    try:
        import UnityPy
    finally:
        sys.path.pop(0)
    archive = source / '_private/original/Hollow Knight/hollow_knight_Data/resources.assets'
    sprites = {obj.path_id: obj for obj in UnityPy.load(str(archive)).objects
               if obj.type.name == 'Sprite' and obj.path_id in (3233, 3326)}
    backboard, arrow = (sprites[path_id].read() for path_id in (3233, 3326))
    if (backboard.m_Name, arrow.m_Name) != ('pop_up_backboard', 'map_arrow'):
        raise ValueError('Unexpected original map UI sprites')
    backboard = backboard.image.convert('RGBA')
    arrow_up = arrow.image.convert('RGBA').resize((15, 7), Image.Resampling.LANCZOS)
    widgets = (
        (0, 336, backboard.resize((204, 60), Image.Resampling.LANCZOS)),
        (208, 336, backboard.resize((138, 36), Image.Resampling.LANCZOS)),
        (350, 336, arrow_up),
        (369, 336, arrow_up.transpose(Image.Transpose.ROTATE_180)),
        (388, 336, arrow_up.transpose(Image.Transpose.ROTATE_90)),
        (399, 336, arrow_up.transpose(Image.Transpose.ROTATE_270)),
    )
    for left, top, sprite in widgets:
        if page.crop((left, top, left + sprite.width, top + sprite.height)).getchannel('A').getbbox():
            raise ValueError('Map widget overlaps the room atlas')
        page.paste(sprite, (left, top))


def convert(source: Path, output: Path) -> dict:
    package = source / '_private/converted/map'
    manifest = json.loads((package / 'manifest.json').read_text(encoding='utf-8'))
    if manifest['pages'] != 1 or manifest['page_size'] != [512, 512]:
        raise ValueError('Expected one 512 x 512 source map page')
    Image = room.import_pil()
    with Image.open(package / 'map-page-0.png') as source_image:
        image = source_image.convert('RGBA')
    if image.size != (512, 512):
        raise ValueError('Map atlas extent no longer matches map_data.hpp')
    add_widgets(source, Image, image)
    page = room.quantize_page(Image, image)
    data = room.hpt1(*image.size, bytes(page.getpalette('RGBA')), page.tobytes())
    output.mkdir(parents=True, exist_ok=True)
    (output / 'map-page-0.hpt').write_bytes(data)
    return {'texture_bytes': len(data), 'rooms': len(manifest['rooms'])}


if __name__ == '__main__':
    report = convert(room.source_root(), room.REPO / '_private/converted-psp/map')
    print(f"Map: {report['rooms']} room records, one {report['texture_bytes']}-byte CLUT8 page")
