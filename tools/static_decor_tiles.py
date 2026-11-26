"""Screen-scale normal-alpha band rasterization, preserving the world-space grid."""
import math
import struct

import numpy as np
from PIL import Image

from room_filtering import nearest_rgba4


def raster_tiles(group, sprites, pages, oversample=2):
    # Same camera FOV/depth as RoomRenderer. The grid lives in world space;
    # moving the camera changes the projection, never the band's parallax.
    density = oversample * 240 / (2 * math.tan(math.radians(25.6) / 2)) / (38.1 + group[0][2][0])
    if not math.isfinite(density) or density <= 0:
        raise ValueError('invalid band depth')
    parts = []
    for _, entry, _ in group:
        sprite = sprites[entry['sprite']]
        page, x, y, w, h, left, bottom, right, top = sprite
        v = entry['values']
        if v[1] or v[3] or any(v[13:]) or v[0] == 0 or v[4] == 0:
            raise ValueError('unsupported material or tilted band')
        x0, x1 = v[2] + v[0]*left, v[2] + v[0]*right
        y0, y1 = v[5] + v[4]*bottom, v[5] + v[4]*top
        parts.append((pages[page], x, y, w, h, x0, x1, y0, y1, v[7:11]))
    left = math.floor(min(min(p[5:7]) for p in parts)*density)
    right = math.ceil(max(max(p[5:7]) for p in parts)*density)
    bottom = math.floor(min(min(p[7:9]) for p in parts)*density)
    top = math.ceil(max(max(p[7:9]) for p in parts)*density)
    if (right-left)*(top-bottom) > 16*1024*1024:
        raise ValueError('band exceeds offline area limit')
    result = []
    for row in range(bottom, top, 1022):
        for col in range(left, right, 1022):
            w, h = min(1022, right-col), min(1022, top-row)
            xx = (np.arange(col-1, col+w+1, dtype=np.float32)+.5)/density
            yy = (np.arange(row+h, row-2, -1, dtype=np.float32)+.5)/density
            rgb = np.zeros((h+2, w+2, 3), dtype=np.float32)
            alpha = np.zeros((h+2, w+2, 1), dtype=np.float32)
            for page, sx, sy, sw, sh, x0, x1, y0, y1, tint in parts:
                # A draw covers its quad; the sampler reads the atlas gutter.
                valid_x = (xx >= min(x0, x1)) & (xx < max(x0, x1))
                valid_y = (yy >= min(y0, y1)) & (yy < max(y0, y1))
                if not valid_x.any() or not valid_y.any():
                    continue
                u = sx + (xx-x0)/(x1-x0)*sw - .5
                v = sy + (y1-yy)/(y1-y0)*sh - .5
                u0, v0 = np.floor(u).astype(int), np.floor(v).astype(int)
                fu, fv = (u-u0).astype('float32')[None, :, None], (v-v0).astype('float32')[:, None, None]
                a = page[np.clip(v0, 0, page.shape[0]-1)[:, None], np.clip(u0, 0, page.shape[1]-1)[None, :]]
                b = page[np.clip(v0, 0, page.shape[0]-1)[:, None], np.clip(u0+1, 0, page.shape[1]-1)[None, :]]
                c = page[np.clip(v0+1, 0, page.shape[0]-1)[:, None], np.clip(u0, 0, page.shape[1]-1)[None, :]]
                d = page[np.clip(v0+1, 0, page.shape[0]-1)[:, None], np.clip(u0+1, 0, page.shape[1]-1)[None, :]]
                sample = ((a*(1-fu)+b*fu)*(1-fv)+(c*(1-fu)+d*fu)*fv)*np.asarray(tint, dtype='float32')
                sa = sample[:, :, 3:4]*valid_y[:, None, None]*valid_x[None, :, None]
                rgb = sample[:, :, :3]*sa + rgb*(1-sa)
                alpha = sa + alpha*(1-sa)
            rgba = np.concatenate((np.divide(rgb, alpha, out=np.zeros_like(rgb), where=alpha > 0), alpha), axis=2)
            image = nearest_rgba4(Image.fromarray(np.clip(np.rint(rgba*255), 0, 255).astype('uint8'), 'RGBA'))
            occupied = image.getchannel('A').crop((1, 1, w+1, h+1)).getbbox()
            if occupied:
                x0, y0, x1, y1 = occupied
                image = image.crop((x0, y0, x1+2, y1+2))
                bounds = ((col+x0)/density, (row+h-y1)/density,
                          (col+x1)/density, (row+h-y0)/density)
                result.append((image, bounds))
    if not result or len(result) >= len(group):
        raise ValueError('band does not reduce draw count')
    return result


def tile_bank(data, accepted, placements):
    magic, pages, sprites, draws, grass = struct.unpack_from('<4s4I', data)
    sprite_end, draw_end = 20+sprites*36, 20+sprites*36+draws*72
    sprite_data, draw_data = bytearray(data[20:sprite_end]), bytearray(data[sprite_end:draw_end])
    cursor, used = 0, set()
    for group, tiles in accepted:
        if len(tiles) >= len(group):
            raise ValueError('nonreducing band')
        if any(index in used for index, _, _ in group):
            raise ValueError('overlapping bands')
        used.update(index for index, _, _ in group)
        for slot, (index, entry, _) in enumerate(group):
            values = list(entry['values'])
            if slot < len(tiles):
                image, bounds = tiles[slot]
                page, x, y = placements[cursor]
                sprite_data.extend(struct.pack('<5I4f', page, x, y, image.width-2, image.height-2, *bounds))
                values[:6] = [1, 0, 0, 0, 1, 0]
                values[7:11] = [1, 1, 1, 1]
                struct.pack_into('<2I16f', draw_data, index*72, sprites+cursor, entry['owner'], *values)
                cursor += 1
            else:
                struct.pack_into('<f', draw_data, index*72+8+10*4, 0)
    count = max((p[0] for p in placements), default=pages-1)+1
    return struct.pack('<4s4I', magic, count, sprites+cursor, draws, grass)+sprite_data+draw_data+data[draw_end:]


def texture_arrays(pages):
    return {key: np.asarray(nearest_rgba4(page), dtype=np.float32)/255 for key, page in pages.items()}
