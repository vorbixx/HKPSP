"""Bake static coplanar Darkness Camera cutouts into SceneBorder alpha.

Approximation: the source border samples a separate camera's alpha. Reuse the
authored layer-1 vignette alpha at startup, without another New 3DS render pass.
Camera blur and moving masks are not reproduced by this conversion-time bake.
"""
import math

from PIL import Image, ImageChops


def cutout_masks(mapping, read, tiles, by_sprite, project):
    masks = []
    for item in mapping['inventory']:
        if not item['active'] or read(item['go'])['m_Layer'] != 1:
            continue
        if not any(read(key)['m_Name'] == 'Sprites-Darkness-Cutout'
                   for key in item['materials']):
            continue
        renderer = read(item['renderer'])
        if not renderer['m_Enabled']:
            continue
        chain = [read(key) for key in item['chain']]
        origin = project((0, 0, 0), chain)
        px, py = project((1, 0, 0), chain), project((0, 1, 0), chain)
        if max(abs(origin[2]), abs(px[2]), abs(py[2])) > .001:
            continue  # A parallax mask cannot be represented by one static bake.
        a, c = px[0]-origin[0], px[1]-origin[1]
        b, d = py[0]-origin[0], py[1]-origin[1]
        if renderer['m_FlipX']:
            a, c = -a, -c
        if renderer['m_FlipY']:
            b, d = -b, -d
        det = a*d-b*c
        if abs(det) < 1e-10:
            continue
        alpha = renderer['m_Color']['m_A']
        for index in by_sprite.get(item['sprite'], ()):
            tile = tiles[index]
            left, bottom, right, top = tile['geometry']
            image = tile['bitmap'].getchannel('A').point(lambda v: round(v*alpha))
            corners = [(origin[0]+a*x+b*y, origin[1]+c*x+d*y)
                       for x in (left, right) for y in (bottom, top)]
            # World XY -> source texture pixels, including reflection/rotation.
            sx, sy = image.width/(right-left), image.height/(top-bottom)
            inverse = (d/det*sx, -b/det*sx,
                       (-d*origin[0]+b*origin[1])/det*sx-left*sx,
                       c/det*sy, -a/det*sy,
                       (-c*origin[0]+a*origin[1])/det*sy+top*sy)
            masks.append(dict(image=image, inverse=inverse,
                              bounds=(min(x for x,y in corners), min(y for x,y in corners),
                                      max(x for x,y in corners), max(y for x,y in corners))))
    return masks


def border_parts(bounds, masks, pixels_per_unit=12):
    """Yield bounds and optional black RGBA tiles; untouched areas stay solid."""
    left, bottom, z, right, top, _ = bounds
    masks = [m for m in masks if m['bounds'][0] < right and m['bounds'][2] > left
             and m['bounds'][1] < top and m['bounds'][3] > bottom]
    if not masks:
        yield (left, bottom, right, top), None
        return
    xs, ys = {left, right}, {bottom, top}
    for mask in masks:
        x0,y0,x1,y1 = mask['bounds']
        xs.update((max(left,x0), min(right,x1)))
        ys.update((max(bottom,y0), min(top,y1)))
    xs, ys = sorted(xs), sorted(ys)
    for x0,x1 in zip(xs, xs[1:]):
        for y0,y1 in zip(ys, ys[1:]):
            overlaps = [m for m in masks if m['bounds'][0] < x1 and m['bounds'][2] > x0
                        and m['bounds'][1] < y1 and m['bounds'][3] > y0]
            if not overlaps:
                yield (x0,y0,x1,y1), None
                continue
            nx, ny = math.ceil((x1-x0)*pixels_per_unit/256), math.ceil((y1-y0)*pixels_per_unit/256)
            for ix in range(nx):
                for iy in range(ny):
                    l,r = x0+(x1-x0)*ix/nx, x0+(x1-x0)*(ix+1)/nx
                    b,t = y0+(y1-y0)*iy/ny, y0+(y1-y0)*(iy+1)/ny
                    w,h = max(1,math.ceil((r-l)*pixels_per_unit)), max(1,math.ceil((t-b)*pixels_per_unit))
                    mask = Image.new('L', (w,h))
                    for source in overlaps:
                        a0,a1,a2,b0,b1,b2 = source['inverse']
                        transform = (a0*(r-l)/w, -a1*(t-b)/h, a0*l+a1*t+a2,
                                     b0*(r-l)/w, -b1*(t-b)/h, b0*l+b1*t+b2)
                        sample = source['image'].transform((w,h), Image.Transform.AFFINE,
                                                         transform, resample=Image.Resampling.BILINEAR)
                        mask = ImageChops.screen(mask, sample)
                    alpha = ImageChops.invert(mask)
                    if not alpha.getbbox():
                        continue
                    if alpha.getextrema() == (255,255):
                        yield (l,b,r,t), None
                    else:
                        image = Image.new('RGBA', (w,h))
                        image.putalpha(alpha)
                        yield (l,b,r,t), image
