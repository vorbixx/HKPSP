"""Pixel-preserving subdivision with source-neighbour filtering gutters."""


def compact_parts(image, geometry, limit=254):
    if not 1 <= limit <= 1022:
        raise ValueError('Invalid compact tile size')
    left, bottom, right, top = geometry
    dx, dy = (right-left)/image.width, (top-bottom)/image.height
    for y in range(0, image.height, limit):
        for x in range(0, image.width, limit):
            r, b = min(x+limit, image.width), min(y+limit, image.height)
            # Include neighbouring texels: an otherwise empty partition can
            # still contain the half-texel fringe of a visible neighbour.
            expanded = image.crop((x-1, y-1, r+1, b+1))
            bounds = expanded.getchannel('A').getbbox()
            if bounds is None:
                continue
            l, t = max(x, x+bounds[0]-2), max(y, y+bounds[1]-2)
            r, b = min(r, x+bounds[2]), min(b, y+bounds[3])
            if l >= r or t >= b:
                continue
            yield dict(bitmap=image.crop((l, t, r, b)),
                       border=image.crop((l-1, t-1, r+1, b+1)),
                       geometry=(left+l*dx, top-b*dy, left+r*dx, top-t*dy),
                       source_rect=(l, t, r, b))
