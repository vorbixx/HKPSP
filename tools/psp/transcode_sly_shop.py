"""Convert Sly's stock and settled original ornaments to one CLUT8 page."""
import hashlib
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import transcode_room as room


PAGE_SIZE = (256, 128)
ORNAMENTS = (
    ('Shop_Menu_top0008', 'Sly Shop Top', (184, 40), (1, 1), True),
    ('Shop_Menu_bottom0008', 'Sly Shop Bottom', (186, 49), (1, 43), False),
    ('Shop_Menu_figurehead_sly0006', 'Sly Shop Figurehead', (27, 34), (189, 1), True),
)


def ornament_images(source, image):
    sys.path.insert(0, str(source / '_private/tools/boss-python'))
    import UnityPy

    asset = source / '_private/original/Hollow Knight/hollow_knight_Data/sharedassets10.assets'
    assets = UnityPy.load(str(asset)).objects
    collection = next(obj for obj in assets if obj.path_id == 621).get_raw_data()
    texture = next(obj for obj in assets if obj.path_id == 15).read()
    if texture.m_Name != 'atlas0' or (texture.m_Width, texture.m_Height) != (2048, 2048):
        raise ValueError('Unexpected original shop atlas')
    atlas = texture.image.convert('RGBA')
    for name, key, size, position, rotated in ORNAMENTS:
        encoded = name.encode('ascii')
        entry = collection.index(struct.pack('<I', len(encoded)) + encoded)
        # In the frozen tk2d definition, 128 bytes follow the padded name before the UV pairs.
        uvs = struct.unpack_from('<8f', collection, entry + 4 + ((len(encoded) + 3) & ~3) + 128)
        xs, ys = uvs[::2], uvs[1::2]
        if (len(set(xs)) != 2 or len(set(ys)) != 2 or not all(0 <= uv <= 1 for uv in uvs)
                or (xs[0] == xs[1]) != rotated):
            raise ValueError(f'Unexpected UVs for {name}')
        left, right = (round(u * atlas.width) for u in (min(xs), max(xs)))
        top, bottom = (round((1 - v) * atlas.height) for v in (max(ys), min(ys)))
        sprite = atlas.crop((left, top, right, bottom))
        if rotated:
            sprite = sprite.transpose(image.Transpose.TRANSVERSE)
        yield key, position, sprite.resize(size, image.Resampling.LANCZOS)


def convert(source, output):
    package = source / '_private/converted/sly-shop'
    table = room.AnimationTable((package / 'sly-shop-icons.bin').read_bytes())
    image = room.import_pil()
    if table.pages != 1 or len(table.clips) != 8 or len(table.sprites) != 8 or any(
            len(indices) != 1 for indices in table.clips.values()):
        raise ValueError('Invalid original Sly icon atlas')
    provenance = json.loads((package / 'icons-source.json').read_text())
    stock = source / provenance['source']
    keys = {indices[0]: key.decode('ascii') for key, indices in table.clips.items()}
    atlas = image.new('RGBA', PAGE_SIZE)
    sprites, clips, frames = [], bytearray(table.clip_bytes), list(table.frames)
    next_x = 1
    for index in range(len(table.sprites)):
        key = keys[index]
        filename = key + '.png'
        path = stock / filename
        if hashlib.sha256(path.read_bytes()).hexdigest() != provenance['sha256'][filename]:
            raise ValueError('Original Sly icon changed: ' + key)
        sprite = image.open(path).convert('RGBA')
        sprite = sprite.crop(sprite.getchannel('A').getbbox())
        sprite.thumbnail((34, 34), image.Resampling.LANCZOS)
        width, height = sprite.size
        lantern = key == 'hasLantern'
        left, top = (218, 1) if lantern else (next_x, 93)
        if left + width >= atlas.width or top + height >= atlas.height:
            raise ValueError('Sly stock exceeds the CLUT8 page')
        atlas.paste(sprite, (left, top))
        sprites.append((0, left, top, width, height, -width/2, -height/2, width/2, height/2))
        if not lantern:
            next_x += width + 2
    for key, (x, y), sprite in ornament_images(source, image):
        atlas.paste(sprite, (x, y))
        sprites.append((0, x, y, *sprite.size, 0, 0, 1, 1))
        clips += struct.pack('<48sf4I', key.encode('ascii'), 1, 0, 0, len(frames), 1)
        frames.append(len(sprites) - 1)
    quant = room.quantize_page(image, atlas)
    palette = bytes(quant.getpalette(rawmode='RGBA'))
    texture = room.hpt1(*PAGE_SIZE, palette, quant.tobytes())
    clip_count = table.n_clips + len(ORNAMENTS)
    header = struct.pack('<4s6I', b'PSA1', 1, room.PAGE, 1, len(sprites), clip_count, len(frames))
    sprite_bytes = b''.join(struct.pack('<5I4f', *sprite) for sprite in sprites)
    frame_bytes = struct.pack(f'<{len(frames)}I', *frames)
    output.mkdir(parents=True, exist_ok=True)
    (output / 'sly-shop-icons.psp.bin').write_bytes(header + sprite_bytes + clips + frame_bytes)
    (output / 'page-0.hpt').write_bytes(texture)
    return clip_count, len(texture)


if __name__ == '__main__':
    clips, size = convert(room.source_root(), room.REPO / '_private/converted-psp/sly-shop')
    print(f'Sly shop: {clips} clips, 1 CLUT8 page ({size} bytes)')
