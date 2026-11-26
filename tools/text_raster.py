"""LCD-sized text and lossless mixed color/luminance atlas packing."""
import math
from PIL import Image, ImageChops, ImageFilter
from atlas_packing import pack_pages
from convert_geo_visuals import pack_sprites

# These sizes are baked independently; the LCD never scales a reduced glyph.
PROMPT_SCALES = (1.15, .92, 1.4)


def source_detail(bitmap, geometry, ppu, minimum_scale=.25):
    """Sample source art once, retaining at least one texel per final LCD pixel.

    Bounds and pivots remain logical; never manufacture detail beyond the source.
    The minimum preserves existing sampling for sprites drawn below HUD scale.
    """
    left, bottom, right, top = geometry
    size = tuple(min(n, max(1, round(n*minimum_scale), math.ceil(abs(span)*ppu)))
                 for n, span in zip(bitmap.size, (right-left, top-bottom)))
    return bitmap.convert('RGBa').resize(size, Image.Resampling.BOX).convert('RGBA')


def lcd_text(bitmap, geometry, ppu=16, raster_scale=1, *, snap_bearings=False):
    """Bake a one-pixel edge; bitmap fonts may opt into raster-aligned bearings."""
    left, _, _, top = geometry
    ppu *= raster_scale
    # The final positioned glyph is snapped by the renderer. Rounding its
    # bearing here as well can shift line endpoints when density changes.
    left, top = left*ppu, top*ppu
    if snap_bearings:
        left, top = round(left), round(top)
    face = Image.new('RGBA', (bitmap.width+2, bitmap.height+2))
    face.paste(bitmap, (1, 1))
    edge = face.getchannel('A').filter(ImageFilter.MaxFilter(3))
    shadow = Image.new('RGBA', face.size, 'black')
    shadow.putalpha(edge.point(lambda a: round(a*.7)))
    bitmap = Image.alpha_composite(shadow, face)
    left, top = left-1, top+1
    return bitmap, (left/ppu, (top-bitmap.height)/ppu,
                    (left+bitmap.width)/ppu, top/ppu)


def sdf_glyph(atlas, glyph, font, pixels, raster_scale=1):
    """Integrate the original SDF midpoint contour at the final LCD size."""
    scale, pad = pixels/font['m_fontInfo']['PointSize'], 4
    x, y = round(glyph['x'])-pad, round(glyph['y'])-pad
    w, h = round(glyph['width'])+pad*2, round(glyph['height'])+pad*2
    mask = atlas.crop((x, y, x+w, y+h)).point(lambda a: 255 if a >= 128 else 0)
    mask = mask.resize((max(1, round(w*scale*raster_scale)),
                        max(1, round(h*scale*raster_scale))), Image.Resampling.LANCZOS)
    bitmap = Image.new('RGBA', mask.size, 'white')
    bitmap.putalpha(mask)
    left, top = (glyph['xOffset']-pad)*scale/16, (glyph['yOffset']+pad)*scale/16
    return bitmap, (left, top-mask.height/(16*raster_scale),
                    left+mask.width/(16*raster_scale), top)


def mixed_pages(sprites, color_sizes, mono_sizes, *, balance=False):
    """Keep colored art byte-exact; monochrome pages retain 8-bit alpha."""
    groups = [[], []]
    for key, bitmap, geometry in sprites:
        r, g, b, _ = bitmap.split()
        mono = not ImageChops.difference(r, g).getbbox() and not ImageChops.difference(g, b).getbbox()
        groups[int(mono)].append((key, bitmap, geometry))
    if balance:
        # Monochrome artwork is also lossless on RGBA8. Move the largest
        # monochrome rectangles to spare color-page space without resampling.
        capacity = [sum(w*h for w, h in sizes) for sizes in (color_sizes, mono_sizes)]
        area = lambda group: sum((im.width+1)*(im.height+1) for _, im, _ in group)
        for sprite in sorted(groups[1], key=lambda s: -s[1].width*s[1].height):
            used = [area(group) for group in groups]
            size = (sprite[1].width+1)*(sprite[1].height+1)
            before = abs(used[0]/capacity[0] - used[1]/capacity[1])
            after = abs((used[0]+size)/capacity[0] - (used[1]-size)/capacity[1])
            if used[0]+size <= capacity[0] and after < before:
                groups[1].remove(sprite)
                groups[0].append(sprite)
    records, pages, formats = {}, [], []
    for group, sizes, fmt in zip(groups, (color_sizes, mono_sizes), ('rgba8', 'la8')):
        if len(sizes) == 1:
            # One pass avoids repeatedly repacking the large glyph inventory.
            images, packed = [Image.new('RGBA', sizes[0])], {}
            pack_sprites(group, images[0], 0, packed, shared_gutters=True)
        else:
            packed, images = pack_pages(group, sizes)
        offset = len(pages)
        records.update({key: (s[0]+offset, *s[1:]) for key, s in packed.items()})
        pages.extend(images)
        formats.extend([fmt]*len(images))
    return records, pages, formats


def texture_bytes(pages, formats):
    return sum(p.width*p.height*(2 if f == 'la8' else 4) for p, f in zip(pages, formats))
