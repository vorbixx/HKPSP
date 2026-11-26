"""HFX4/FOC1 density bounds, mirroring the shared C++ loaders and draws.

Sidecars name AnimationBank clips rather than sprite indices. The optional
``clips`` mapping supplies those clips' frame indices from the companion HKA1.
All effects draw with worldPpu (hero plane, z=0) in src/main.cpp.
"""
import math
import struct
import re


class Reader:
    def __init__(self, data, magic):
        self.data, self.pos = data, 4
        if data[:4] != magic:
            raise ValueError(f"{magic!r}: magic")

    def read(self, fmt):
        size = struct.calcsize('<' + fmt)
        if self.pos + size > len(self.data):
            raise ValueError('effect table truncated')
        result = struct.unpack_from('<' + fmt, self.data, self.pos)
        self.pos += size
        return result

    def floats(self, count):
        values = self.read(f'{count}f')
        require(all(map(math.isfinite, values)), 'nonfinite effect value')
        return values

    def name(self):
        raw, = self.read('48s')
        require(b'\0' in raw, 'unterminated effect clip name')
        return raw.split(b'\0', 1)[0]

    def end(self):
        require(self.pos == len(self.data), 'effect table size')


def require(valid, message):
    if not valid:
        raise ValueError(message)


def add_clip(result, clips, name, sprite_count, scale):
    require(0 < sprite_count <= 8192, 'effect sprite count')
    require(clips is not None and name in clips, f'missing effect clip {name!r}')
    frames = clips[name]
    require(bool(frames), 'empty effect clip')
    for sprite in frames:
        require(0 <= sprite < sprite_count, 'effect sprite index')
        previous = result.get(sprite, (0, 0))
        result[sprite] = tuple(max(a, abs(b)) for a, b in zip(previous, scale))


def hermite_max(keys):
    """Absolute cubic extrema, including overshoot between the three keys."""
    bound = max(abs(k[1]) for k in keys)
    for left, right in zip(keys, keys[1:]):
        dt = right[0] - left[0]
        y0, y1, m0, m1 = left[1], right[1], dt * left[3], dt * right[2]
        a, b, c = 2*y0 - 2*y1 + m0 + m1, -3*y0 + 3*y1 - 2*m0 - m1, m0
        if a == 0:
            roots = [-c / (2*b)] if b else []
        else:
            discriminant = 4*b*b - 12*a*c
            roots = [(-2*b + sign*math.sqrt(discriminant))/(6*a)
                     for sign in (-1, 1)] if discriminant >= 0 else []
        for t in roots:
            if 0 < t < 1:
                bound = max(bound, abs(((a*t + b)*t + c)*t + y0))
    return bound


def hero_effects(data, sprite_count, clips=None):
    """Return {sprite: max absolute (scale X, scale Y)} for HFX4."""
    r, result = Reader(data, b'HFX4'), {}
    require(r.read('I')[0] == 8, 'hero effect count')
    for index, expected_owner in enumerate((4217, 6465, 7661, 8103, 8243, 3992, 4359, 5629)):
        owner, shape, tiles, flags = r.read('4I')
        v = r.floats(86)
        fling = index >= 6
        require(owner == expected_owner and 0 < tiles <= (7 if fling else 3) and
                ((flags == 4 and shape == 0) if fling else
                 (flags <= 3 and shape in (4, 5, 10, 12))), 'hero effect fields')
        require(0 < v[0] <= 2 and v[1] >= 0 and 0 < v[2] <= v[3] <= 5,
                'hero effect lifetime')
        if fling:
            require(v[49] > 0 and v[0] == v[2] == v[3] and
                    0 <= v[4] <= v[5] and v[6] > 0 and v[8] <= v[9] and v[84] <= v[85] and
                    1 <= v[82] <= v[83] <= 3 and v[82].is_integer() and v[83].is_integer(),
                    'hero fling values')
        # Fling returns before setting mix: drawSize uses only the low curve.
        size = (abs(v[6]) * max(map(abs, v[50:58])) if fling else
                max(abs(v[6]), abs(v[7])) * max(map(abs, v[50:66])))
        for tile in range(tiles):
            add_clip(result, clips, f'Hero FX {owner} {tile}'.encode(), sprite_count, (size, size))
    flash = r.floats(7)
    curves = []
    for channel in range(3):
        keys, previous = [], -1
        for _ in range(3):
            key = r.floats(4)
            require(previous < key[0] <= flash[4] and 0 <= key[1] <= (1 if channel == 2 else 10),
                    'hero flash curve')
            keys.append(key)
            previous = key[0]
        require(keys[0][0] == 0, 'hero flash curve origin')
        curves.append(keys)
    require(flash[2] > 0 and flash[3] > 0 and 0 < flash[4] <= 1 and flash[5] <= flash[6],
            'hero flash values')
    add_clip(result, clips, b'Hero Damage Flash', sprite_count,
             (max(flash[2], hermite_max(curves[0])), max(flash[3], hermite_max(curves[1]))))
    r.end()
    return result


def focus_effects(data, sprite_count, clips=None):
    """Return {sprite: max absolute (scale X, scale Y)} for FOC1."""
    r, result = Reader(data, b'FOC1'), {}
    sprites, emitters = r.read('2H')
    require(0 < sprites <= 5 and emitters <= 4, 'focus effect counts')
    kinds = set()
    for _ in range(sprites):
        name, kind = r.name(), r.read('I')[0]
        v = r.floats(6)
        require(kind < 5 and kind not in kinds and
                (v[4] >= 0 if kind in (0, 3) else v[4] > 0) and v[5] >= 0,
                'focus sprite values')
        kinds.add(kind)
        add_clip(result, clips, name, sprite_count, v[2:4])
    for _ in range(emitters):
        name = r.name()
        r.read('I')  # Shape is unrestricted by FocusEffects::load.
        v = r.floats(15)
        require(0 <= v[0] <= 1000 and 0 < v[1] <= v[2] <= 10 and
                v[3] >= 0 and v[4] >= 0 and v[5] >= 0 and v[7] >= v[6] and
                v[9] >= v[8] and 0 < v[10] <= v[11] and v[14] >= 0, 'focus emitter values')
        add_clip(result, clips, name, sprite_count, (v[11], v[11]))
    r.end()
    return result


def nail_shapes(data, sprite_count, clips=None):
    """HKN1: each slash has an authored (independent) XY draw scale."""
    r, result = Reader(data, b'HKN1'), {}
    require(r.read('I')[0] == 5, 'nail variant count')
    for _ in range(5):
        v = r.floats(4)
        _, vertices = r.read('2I')
        name = r.name()
        require(3 <= vertices <= 16 and v[2] != 0 and v[3] != 0, 'nail transform')
        add_clip(result, clips, name, sprite_count, v[2:4])
        r.floats(2*vertices)
    r.end()
    return result


def death_flash_scales(source, sprite_count, clips):
    """Use the shared presenter's actual Hermite keys, including overshoot.

    Death has no separate binary sidecar. This is intentionally a checked parse
    of the runtime curves instead of a second hand-maintained transform table.
    """
    body = source.split('float HeroDeathPresentation::flashScale(bool rays) const', 1)[1].split('float HeroDeathParticle::drawSize()', 1)[0]
    result = {}
    for name, key in ((b'Death Flash', 'ring'), (b'Death Flash Rays', 'ray')):
        match = re.search(r'const CurveKey '+key+r'\[\]=\{(.*?)\};', body, re.S)
        require(match is not None, 'death curve declaration changed')
        values = [float(v.rstrip('f')) for v in re.findall(r'[-+]?(?:\d*\.\d+|\d+)(?:e[-+]?\d+)?f?', match[1])]
        require(len(values) >= 8 and len(values) % 4 == 0, 'death curve keys')
        scale = hermite_max([values[i:i+4] for i in range(0, len(values), 4)])
        add_clip(result, clips, name, sprite_count, (scale, scale))
    return result
