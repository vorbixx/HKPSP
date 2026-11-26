"""Experimental offline room-layer compositing; never modifies runtime assets."""
import argparse
import json
import math
import shutil
import struct
import subprocess
from pathlib import Path

from PIL import Image, ImageChops
from room_filtering import nearest_rgba4
from tool_paths import TEX3DS as TEX3DS_EXE

ROOT = Path(__file__).resolve().parents[1]


def load_bank(path):
    data = path.read_bytes()
    if len(data) < 20:
        raise ValueError('Truncated room bank')
    magic, pages, tiles, draws, grass = struct.unpack_from('<4s4I', data)
    footer = 4 if magic == b'HKR4' else 0
    if (magic not in (b'HKR2', b'HKR3', b'HKR4') or (magic == b'HKR2' and grass != 0) or
            len(data) != 20 + 36 * tiles + 72 * draws + 24 * grass + footer):
        raise ValueError('Invalid HKR2/HKR3/HKR4 room bank')
    if footer and not math.isfinite(struct.unpack_from('<f', data, len(data)-4)[0]):
        raise ValueError('Invalid room depth split')
    offset = 20
    sprites = []
    for index in range(tiles):
        sprite = struct.unpack_from('<5I4f', data, offset)
        if not all(map(math.isfinite, sprite[5:])):
            raise ValueError('Invalid sprite geometry')
        sprites.append(sprite)
        offset += 36
    entries = []
    for index in range(draws):
        sprite, owner, *values = struct.unpack_from('<2I16f', data, offset)
        offset += 72
        if sprite >= tiles or sprites[sprite][0] >= pages or not all(map(math.isfinite, values)):
            raise ValueError('Invalid draw or sprite reference')
        entries.append(dict(sprite=sprite, owner=owner, values=values))
    animated = set()
    for index in range(grass):
        draw_index = struct.unpack_from('<I', data, offset)[0]
        offset += 24
        if draw_index >= draws:
            raise ValueError('Invalid grass draw index')
        animated.add(draw_index)
    return sprites, entries, animated


def candidates(entries, animated, owners, same_owner_only=False):
    groups, group = [], []
    for index, entry in enumerate(entries):
        values = entry['values']
        admissible = (index not in animated and entry['owner'] in owners and
                      values[1] == values[3] == values[13] == values[14] == values[15] == 0 and
                      values[0] != 0 and values[4] != 0 and
                      all(0 <= channel <= 1 for channel in values[7:11]))
        key = tuple(values[position] for position in (6, 11, 12))
        if not admissible or (group and (group[0][2] != key or
                (same_owner_only and group[0][1]['owner'] != entry['owner']))):
            if len(group) > 1:
                groups.append(group)
            group = []
        if admissible:
            group.append((index, entry, key))
    if len(group) > 1:
        groups.append(group)
    return groups


def coverage_estimate(group, sprites):
    rectangles = []
    for _, entry, _ in group:
        _, _, _, _, _, left, bottom, right, top = sprites[entry['sprite']]
        values = entry['values']
        x0, x1 = sorted((values[2] + values[0] * left, values[2] + values[0] * right))
        y0, y1 = sorted((values[5] + values[4] * bottom, values[5] + values[4] * top))
        if x0 >= x1 or y0 >= y1:
            raise ValueError('Invalid sprite geometry')
        rectangles.append((x0, y0, x1, y1))
    original = sum((right - left) * (top - bottom) for left, bottom, right, top in rectangles)
    composite = ((max(rect[2] for rect in rectangles) - min(rect[0] for rect in rectangles)) *
                 (max(rect[3] for rect in rectangles) - min(rect[1] for rect in rectangles)))
    return original, composite, composite / original


def exact_tile_composite(group, sprites, pages):
    """Join rectangular tiles only when their RGBA4 filtering footprints agree."""
    if len(group) < 2:
        raise ValueError('At least two tiles are required')
    if any(group[index][0] != group[0][0] + index for index in range(len(group))):
        raise ValueError('Tiles must be consecutive draws')
    first = group[0][1]
    tint, key = first['values'][7:11], group[0][2]
    parts, density = [], None
    for _, entry, entry_key in group:
        values = entry['values']
        if (entry['owner'] != first['owner'] or entry_key != key or
                values[7:11] != tint or values[1] != 0 or values[3] != 0 or
                values[13:16] != [0, 0, 0] or values[0] <= 0 or values[4] <= 0):
            raise ValueError('Tiles must share owner, tint, depth and normal axis-aligned rendering')
        page, x, y, width, height, left, bottom, right, top = sprites[entry['sprite']]
        if (page not in pages or min(x, y, width, height) < 1 or
                x + width >= pages[page].width or y + height >= pages[page].height or
                left >= right or bottom >= top):
            raise ValueError('Tile requires a one-pixel filtering border')
        x0, x1 = values[2] + values[0]*left, values[2] + values[0]*right
        y0, y1 = values[5] + values[4]*bottom, values[5] + values[4]*top
        current = (width/(x1-x0), height/(y1-y0))
        if density is None:
            density = current
        if any(not math.isclose(a, b, rel_tol=1e-6, abs_tol=1e-6)
               for a, b in zip(density, current)):
            raise ValueError('Tile texel densities differ')
        parts.append((pages[page], (x, y, width, height), (x0, y0, x1, y1)))
    bounds = (min(p[2][0] for p in parts), min(p[2][1] for p in parts),
              max(p[2][2] for p in parts), max(p[2][3] for p in parts))
    def pixel(value):
        rounded = round(value)
        if not math.isclose(value, rounded, rel_tol=0, abs_tol=1e-4):
            raise ValueError('Tile edges are not texel-aligned')
        return rounded
    width = pixel((bounds[2]-bounds[0])*density[0])
    height = pixel((bounds[3]-bounds[1])*density[1])
    if not 0 < width <= 1022 or not 0 < height <= 1022:
        raise ValueError('Composite exceeds resident atlas limits')
    image = Image.new('RGBA', (width+2, height+2))
    occupied = Image.new('1', image.size)
    interior = Image.new('1', (width, height))
    for page, (x, y, tile_width, tile_height), extent in parts:
        left = pixel((extent[0]-bounds[0])*density[0])
        top = pixel((bounds[3]-extent[3])*density[1])
        if pixel((extent[2]-bounds[0])*density[0])-left != tile_width or \
                pixel((bounds[3]-extent[1])*density[1])-top != tile_height:
            raise ValueError('Tile extents do not match its texels')
        area = (left, top, left+tile_width, top+tile_height)
        if interior.crop(area).getbbox():
            raise ValueError('Tile interiors overlap')
        interior.paste(1, area)
        bordered = nearest_rgba4(page.crop((x-1, y-1, x+tile_width+1, y+tile_height+1)))
        target = (left, top, left+tile_width+2, top+tile_height+2)
        mismatch = ImageChops.difference(image.crop(target), bordered)
        mask = occupied.crop(target).convert('L')
        if any(ImageChops.multiply(channel, mask).getbbox() for channel in mismatch.split()):
            raise ValueError('Tile filtering borders disagree')
        image.paste(bordered, (left, top))
        occupied.paste(1, target)
    if interior.histogram()[0] != 0:
        raise ValueError('Tiles do not cover a rectangle')
    return image, bounds


def compose(group, sprites, pages, pixels_per_unit, visibility=1, background=(0, 0, 0, 0)):
    if not math.isfinite(pixels_per_unit) or pixels_per_unit <= 0:
        raise ValueError('pixels per unit must be positive and finite')
    if not math.isfinite(visibility) or not 0 <= visibility <= 1:
        raise ValueError('visibility must be between zero and one')
    parts = []
    for index, entry, key in group:
        page, x, y, width, height, left, bottom, right, top = sprites[entry['sprite']]
        values = entry['values']
        if not (0 < width and 0 < height and left < right and bottom < top and
                page in pages and x + width <= pages[page].width and y + height <= pages[page].height):
            raise ValueError('Invalid sprite rectangle')
        x0, x1 = sorted((values[2] + values[0] * left, values[2] + values[0] * right))
        y0, y1 = sorted((values[5] + values[4] * bottom, values[5] + values[4] * top))
        parts.append((entry, (page, x, y, width, height), (x0, y0, x1, y1)))
    bounds = (min(part[2][0] for part in parts), min(part[2][1] for part in parts),
              max(part[2][2] for part in parts), max(part[2][3] for part in parts))
    width = max(1, math.ceil((bounds[2] - bounds[0]) * pixels_per_unit))
    height = max(1, math.ceil((bounds[3] - bounds[1]) * pixels_per_unit))
    if width > 2048 or height > 2048:
        raise ValueError('Composite exceeds 2048-pixel dimension')
    result = Image.new('RGBA', (width, height), background)
    for entry, rect, extent in parts:
        page, x, y, source_width, source_height = rect
        image = pages[page].crop((x, y, x + source_width, y + source_height)).convert('RGBA')
        values = entry['values']
        if values[0] < 0:
            image = image.transpose(Image.Transpose.FLIP_LEFT_RIGHT)
        if values[4] < 0:
            image = image.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
        channels = [ImageChops.multiply(channel, Image.new('L', image.size, round(tint * 255)))
                    for channel, tint in zip(image.split(), values[7:11])]
        image = Image.merge('RGBA', channels)
        if visibility != 1:
            image.putalpha(image.getchannel('A').point(lambda value: round(value * visibility)))
        left = round((extent[0] - bounds[0]) * pixels_per_unit)
        right = round((extent[2] - bounds[0]) * pixels_per_unit)
        top = round((bounds[3] - extent[3]) * pixels_per_unit)
        bottom = round((bounds[3] - extent[1]) * pixels_per_unit)
        if right <= left or bottom <= top:
            raise ValueError('Composite resolution loses a source draw')
        image = image.resize((right - left, bottom - top), Image.Resampling.BILINEAR)
        result.alpha_composite(image, (left, top))
    return result, bounds


def visibility_audit(group, sprites, pages, pixels_per_unit, image, visibility=0.5):
    flattened = image.copy()
    flattened.putalpha(flattened.getchannel('A').point(lambda value: round(value * visibility)))
    result = {}
    for name, background in (('black', (0, 0, 0, 255)), ('white', (255, 255, 255, 255))):
        original_frame, _ = compose(group, sprites, pages, pixels_per_unit, visibility, background)
        flattened_frame = Image.new('RGBA', image.size, background)
        flattened_frame.alpha_composite(flattened)
        difference = ImageChops.difference(original_frame.convert('RGB'), flattened_frame.convert('RGB'))
        histogram = difference.histogram()
        result[name] = dict(max_channel_error=max((index % 256 for index, count in enumerate(histogram)
                                                   if count), default=0),
                            total_channel_error=sum((index % 256) * count
                                                    for index, count in enumerate(histogram)))
    return dict(visibility=visibility, backgrounds=result)


def rgba4_audit(group, sprites, pages, pixels_per_unit, visibility=1, preserve_tint=False):
    quantized_pages = {index: nearest_rgba4(image) for index, image in pages.items()}
    source_group = group
    if preserve_tint:
        tint = group[0][1]['values'][7:11]
        if any(entry['values'][7:11] != tint for _, entry, _ in group):
            raise ValueError('Preserved tint requires identical source tints')
        source_group = []
        for index, entry, key in group:
            values = entry['values'].copy()
            values[7:11] = [1, 1, 1, 1]
            source_group.append((index, dict(entry, values=values), key))
    flattened, _ = compose(source_group, sprites, quantized_pages, pixels_per_unit)
    flattened = nearest_rgba4(flattened)
    if preserve_tint:
        flattened = Image.merge('RGBA', [ImageChops.multiply(channel, Image.new('L', flattened.size, round(value * 255)))
                                         for channel, value in zip(flattened.split(), tint)])
    return visibility_audit(group, sprites, quantized_pages, pixels_per_unit, flattened, visibility)


def _with_filter_border(image):
    """Extend a composed preview by one edge texel on every side."""
    image = image.convert('RGBA')
    width, height = image.size
    if width < 1 or height < 1:
        raise ValueError('Cannot border an empty image')
    bordered = Image.new('RGBA', (width + 2, height + 2))
    bordered.paste(image, (1, 1))
    bordered.paste(image.crop((0, 0, width, 1)), (1, 0))
    bordered.paste(image.crop((0, height - 1, width, height)), (1, height + 1))
    bordered.paste(image.crop((0, 0, 1, height)), (0, 1))
    bordered.paste(image.crop((width - 1, 0, width, height)), (width + 1, 1))
    bordered.putpixel((0, 0), image.getpixel((0, 0)))
    bordered.putpixel((width + 1, 0), image.getpixel((width - 1, 0)))
    bordered.putpixel((0, height + 1), image.getpixel((0, height - 1)))
    bordered.putpixel((width + 1, height + 1), image.getpixel((width - 1, height - 1)))
    return bordered


def prototype_bank(data, composites, placements=None, preserve_tint=False):
    """Append composite sprites and hide their source draws.

    Composite images include a one-pixel filtering border on every side;
    serialized sprite rectangles describe only the interior.  ``placements``
    therefore contains interior ``(page, x, y)`` positions.

    ``placements`` is an optional list of ``(page, x, y)`` atlas positions for
    packed output pages.  The original preview path leaves it unset and keeps
    one composite per page; the static-decor baker supplies packed positions.
    """
    magic, pages, tiles, draw_count, grass = struct.unpack_from('<4s4I', data)
    footer = 4 if magic == b'HKR4' else 0
    if magic not in (b'HKR3', b'HKR4') or len(data) != 20 + 36 * tiles + 72 * draw_count + 24 * grass + footer:
        raise ValueError('Invalid HKR3/HKR4 room bank')
    if footer and not math.isfinite(struct.unpack_from('<f', data, len(data)-4)[0]):
        raise ValueError('Invalid room depth split')
    if placements is not None and len(placements) != len(composites):
        raise ValueError('Composite placement count does not match images')
    sprite_end = 20 + 36 * tiles
    draw_end = sprite_end + 72 * draw_count
    sprite_data = bytearray(data[20:sprite_end])
    draw_data = bytearray(data[sprite_end:draw_end])
    used = set()
    for composite_index, (group, image, bounds) in enumerate(composites):
        indices = [row[0] for row in group]
        if (len(indices) < 2 or len(set(indices)) != len(indices)
                or any(index < 0 or index >= draw_count or index in used for index in indices)
                or image.width < 3 or image.height < 3
                or image.width > 1024 or image.height > 1024):
            raise ValueError('Invalid or overlapping composite')
        used.update(indices)
        if placements is None:
            page, x, y = pages + composite_index, 1, 1
        else:
            page, x, y = placements[composite_index]
            if page < pages or x < 1 or y < 1:
                raise ValueError('Invalid composite placement')
        interior_width, interior_height = image.width - 2, image.height - 2
        border_x, border_y = x - 1, y - 1
        if (page < 0 or border_x < 0 or border_y < 0 or
                border_x + image.width > 1024 or border_y + image.height > 1024):
            raise ValueError('Composite placement exceeds atlas page')
        sprite_data.extend(struct.pack('<5I4f', page, x, y,
                                       interior_width, interior_height, *bounds))
        first = indices[0] * 72
        sprite, owner, *values = struct.unpack_from('<2I16f', draw_data, first)
        values[0:6] = [1, 0, 0, 0, 1, 0]
        if not preserve_tint:
            values[7:11] = [1, 1, 1, 1]
        struct.pack_into('<2I16f', draw_data, first, tiles + composite_index, owner, *values)
        for index in indices[1:]:
            struct.pack_into('<f', draw_data, index * 72 + 8 + 10 * 4, 0)
    added_pages = len(composites) if placements is None else (
        max((placement[0] for placement in placements), default=pages - 1) - pages + 1)
    return (struct.pack('<4s4I', magic, pages + added_pages, tiles + len(composites),
                        draw_count, grass) + sprite_data + draw_data + data[draw_end:])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--enable', action='store_true', help='enable experimental preview output')
    parser.add_argument('--bank', type=Path, required=True)
    parser.add_argument('--owners', type=int, nargs='+', required=True)
    parser.add_argument('--pixels-per-unit', type=float, default=32)
    parser.add_argument('--same-owner-only', action='store_true',
                        help='only combine consecutive draws with the same visibility owner')
    parser.add_argument('--max-area-ratio', type=float, default=None,
                        help='reject groups whose un-clipped composite/source area exceeds this ratio')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--emit-experimental-bank', action='store_true',
                        help='emit an unvalidated experimental bank and RGBA4 atlas pages')
    parser.add_argument('--tex3ds', type=Path, default=Path(TEX3DS_EXE))
    args = parser.parse_args()
    if not args.enable:
        parser.error('offline experiment requires --enable')
    if args.max_area_ratio is not None and (not math.isfinite(args.max_area_ratio) or args.max_area_ratio <= 0):
        parser.error('--max-area-ratio must be positive and finite')
    output = args.output.resolve()
    if not output.is_relative_to((ROOT / 'build' / 'optimise-fog').resolve()):
        parser.error('output must be under build/optimise-fog')
    sprites, entries, animated = load_bank(args.bank)
    bank_data = args.bank.read_bytes()
    groups = candidates(entries, animated, set(args.owners), args.same_owner_only)
    page_indices = {sprites[entry['sprite']][0] for group in groups for _, entry, _ in group}
    pages = {index: Image.open(args.bank.parent / f'room-page-{index}.png').convert('RGBA')
             for index in page_indices}
    output.mkdir(parents=True, exist_ok=True)
    report = []
    composites = []
    for group in groups:
        row = dict(draws=[entry[0] for entry in group], owners=[entry[1]['owner'] for entry in group],
                   depth=group[0][2][0], layer=group[0][2][1], order=group[0][2][2])
        try:
            exact, _ = exact_tile_composite(group, sprites, pages)
            row['exact_tile_screen'] = dict(accepted=True, bordered_size=exact.size)
        except ValueError as error:
            row['exact_tile_screen'] = dict(accepted=False, reason=str(error))
        try:
            original_area, composite_area, area_ratio = coverage_estimate(group, sprites)
            row.update(source_quad_area=original_area, composite_quad_area=composite_area,
                       area_ratio=area_ratio)
            if args.max_area_ratio is not None and area_ratio > args.max_area_ratio:
                raise ValueError('Composite area ratio exceeds limit')
            image, bounds = compose(group, sprites, pages, args.pixels_per_unit)
            row.update(bounds=bounds, size=image.size, image=f'group-{len(report)}.png')
            row['preview_visibility_audit'] = visibility_audit(group, sprites, pages,
                                                               args.pixels_per_unit, image)
            row['rgba4_preview_audit'] = [rgba4_audit(group, sprites, pages, args.pixels_per_unit, visibility)
                                          for visibility in (1, 0.5)]
            if all(entry['values'][7:11] == group[0][1]['values'][7:11] for _, entry, _ in group):
                row['rgba4_preserved_tint_audit'] = [rgba4_audit(group, sprites, pages, args.pixels_per_unit,
                                                                visibility, preserve_tint=True)
                                                     for visibility in (1, 0.5)]
            image.save(output / row['image'])
            if args.emit_experimental_bank and image.width <= 1022 and image.height <= 1022:
                composites.append((group, _with_filter_border(image), bounds))
        except ValueError as error:
            row['rejected'] = str(error)
        report.append(row)
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    if args.emit_experimental_bank:
        page_count = struct.unpack_from('<I', bank_data, 4)[0]
        for index in range(page_count):
            shutil.copyfile(args.bank.parent / f'room-page-{index}.t3x', output / f'room-page-{index}.t3x')
        for index, (_, image, _) in enumerate(composites):
            page = Image.new('RGBA', (1024, 1024))
            page.paste(image, (0, 0))
            png = output / f'room-page-{page_count + index}.png'
            page.save(png)
            subprocess.run([str(args.tex3ds), '-f', 'rgba4', '-z', 'none', '-o',
                            str(output / f'room-page-{page_count + index}.t3x'), str(png)], check=True)
        (output / 'room-visual.bin').write_bytes(prototype_bank(bank_data, composites))
    print(json.dumps(dict(groups=len(groups), composited=sum('image' in row for row in report))))


if __name__ == '__main__':
    main()
