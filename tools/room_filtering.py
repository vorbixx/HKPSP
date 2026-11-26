"""Offline coverage approximation for room artwork on the 400x240 screen."""
import math

from PIL import ImageFilter
from filter_cache import filtered
from room_blur import split_depth


def sprite_footprints(mapping, read, project, animation_sprites=None):
    """Use the least minified instance so shared sprites retain close detail."""
    footprints = {}
    foreground_boundary = split_depth(mapping, read, project)
    focal = 240 / (2 * math.tan(math.radians(25.6) / 2))
    inventory = list(mapping['inventory'])
    for item in mapping['inventory']:
        inventory.extend(dict(item, sprite=key)
                         for key in (animation_sprites or {}).get(item.get('go'), ()))
    for item in inventory:
        key = item['sprite']
        if not item['active'] or key is None:
            continue
        if any(read(n)['m_Name'] == 'SwayingGrass_Crossroads' for n in item['materials']):
            # Authored and animated grass frames must keep the same pixels.
            footprints[key] = 0
            continue
        sprite = read(key)
        chain = [read(n) for n in item['chain']]
        origin = project((0, 0, 0), chain)
        # Match the actual sharp scene pass, including art at/behind the hero.
        # Sprites shared with the blurred composition retain their source pixels.
        if origin[2] >= foreground_boundary:
            footprints[key] = 0
            continue
        px, py = project((1, 0, 0), chain), project((0, 1, 0), chain)
        depth = 38.1 + origin[2]
        if depth <= 0:
            footprints[key] = 0
            continue
        scale = max(math.hypot(p[0]-origin[0], p[1]-origin[1]) for p in (px, py))
        if scale <= 0:
            continue
        footprint = sprite['m_PixelsToUnits'] * depth / (focal * scale)
        footprints[key] = min(footprints.get(key, footprint), footprint)
    return footprints


def prefilter(image, footprint):
    # Approximation: the reference player is captured at 1200x720 and reduced
    # to 400x240. A .35-screen-pixel coverage filter approximates that reduction
    # without another GPU target/pass. Enlarged fog remains at source resolution.
    radius = .35 * math.sqrt(max(0, footprint*footprint-1))
    if radius < .25:
        return image
    # Filter associated color so transparent source texels cannot darken edges.
    return filtered(image, radius, _blur)


def _blur(image, radius):
    return image.convert('RGBa').filter(ImageFilter.GaussianBlur(radius)).convert('RGBA')


def nearest_alpha(image):
    """Pre-round alpha: tex3ds otherwise truncates its four low bits."""
    image = image.copy()
    image.putalpha(image.getchannel('A').point(lambda a: 17*((a+8)//17)))
    return image


def nearest_rgba4(image):
    """Round straight RGBA to representable levels before tex3ds truncation."""
    if image.mode != 'RGBA':
        raise ValueError('RGBA4 rounding requires straight RGBA pixels')
    levels = [17*((value+8)//17) for value in range(256)]
    return image.point(levels*4)
