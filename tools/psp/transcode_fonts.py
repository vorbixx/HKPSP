"""Final-size original UI fonts, PFT1 v1 tables and swizzled CLUT4 HPT1 pages.

PFT1: magic + 6 u32 (version, page size, pages, fonts, glyphs, kern pairs).
Font records <4I4f: id, size px, first glyph, count, ascent, descent, H ink height,
line height. Glyph <7I3f: Unicode, font id, page,x,y,w,h, advance,bearing x,y.
Bearings use Pillow's baseline anchor 'ls' (y down). Kerning <3If: font,left,right,
advance adjustment. All integers/floats little endian. Exact size is required.
HPT1 keeps the existing 16-byte header, but psm=4 (GU_PSM_T4), clut entries=16.
Even x occupies the low nibble; swizzle operates on packed bytes (16 B x 8 rows).
Palette RGB=white, alpha=0,17,...255; vertex colour supplies the text colour.
"""
import html
import json
import math
import struct
import sys
import unicodedata
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import transcode_room as t
from font_ladder import FACES

SIZES = (('perpetua', 15), ('trajan', 11), ('trajan', 14), ('trajan', 18))
# Original Russian outline fonts (resources.assets:1406 and sharedassets0:73), used only as the
# second source of the ordered fallback: English face first, then the RU face of the same role.
RU_FACES = {'perpetua': '_private/research/russian-fonts/Perpetua_ru_edited.ttf',
            'trajan': '_private/research/russian-fonts/TrajanPro-Bold_ru.otf'}
PAGE = 256


def charset(root):
    paths = sorted((root/'_private/research').glob('*/EN_*.xml'))
    if not paths:
        raise ValueError('no English localization sources')
    text = ''.join(html.unescape(''.join(ET.parse(p).getroot().itertext())) for p in paths)
    latin = {ord(c) for c in text if 160 <= ord(c) <= 255 and unicodedata.category(c).startswith('L')}
    return sorted(set(range(32, 127)) | latin), paths


def russian_charset(root):
    """Fallback candidates: every printable code point in the RU sheets, the Cyrillic block and Yo."""
    paths = sorted((root/'_private/research').glob('*/RU_*-decrypted.xml'))
    if not paths:
        raise ValueError('no Russian localization sources')
    text = ''.join(html.unescape(''.join(ET.parse(p).getroot().itertext())) for p in paths)
    sheet = {ord(c) for c in text if unicodedata.category(c) not in ('Cc', 'Cs', 'Co', 'Cn')}
    cyrillic = set(range(0x410, 0x450)) | {0x401, 0x451}
    return sorted(sheet | cyrillic), paths


def ru_text(root, sheet, key):
    """Original Russian text of one entry; <br> line breaks are returned as separate lines."""
    for p in sorted((root/'_private/research').glob(f'*/RU_{sheet}-decrypted.xml')):
        for entry in ET.parse(p).getroot().iter('entry'):
            if entry.get('name') == key:
                return html.unescape(''.join(entry.itertext())).split('<br>')
    raise ValueError(f'no RU entry {sheet}/{key}')


def sfnt(data):
    return {data[12+i*16:16+i*16]: struct.unpack_from('>II', data, 20+i*16)
            for i in range(struct.unpack_from('>H', data, 4)[0])}


def cmap(path):
    """Read Unicode coverage from sfnt cmap 4/12; fail instead of rasterising .notdef."""
    data = path.read_bytes()
    offsets = sfnt(data)
    start, _ = offsets[b'cmap']
    result = {}
    for i in range(struct.unpack_from('>H', data, start+2)[0]):
        platform, encoding, offset = struct.unpack_from('>HHI', data, start+4+8*i)
        if platform != 0 and (platform != 3 or encoding not in (1, 10)):
            continue
        pos = start+offset
        fmt = struct.unpack_from('>H', data, pos)[0]
        if fmt == 12:
            for j in range(struct.unpack_from('>I', data, pos+12)[0]):
                lo, hi, glyph = struct.unpack_from('>3I', data, pos+16+j*12)
                result.update({cp:glyph+cp-lo for cp in range(lo+(glyph == 0), hi+1)})
        elif fmt == 4:
            count = struct.unpack_from('>H', data, pos+6)[0]//2
            for j in range(count):
                end = struct.unpack_from('>H', data, pos+14+2*j)[0]
                begin = struct.unpack_from('>H', data, pos+16+2*count+2*j)[0]
                delta = struct.unpack_from('>h', data, pos+16+4*count+2*j)[0]
                addr = pos+16+6*count+2*j
                span = struct.unpack_from('>H', data, addr)[0]
                for cp in range(begin, min(end, 65534)+1):
                    glyph = struct.unpack_from('>H', data, addr+span+2*(cp-begin))[0] if span else cp
                    if glyph and (glyph+delta)&65535:
                        result[cp] = (glyph+delta)&65535
    return result


def kerning(path, cps, size):
    """Original kern/GPOS advances in font units, scaled once to final pixels.

    Pillow BASIC omits GPOS and divides legacy kerning by 64 again; read the
    actual tables. Only horizontal advance kerning is supported; fail on new
    lookup/value types instead of silently dropping positioning information.
    """
    data = path.read_bytes()
    tables, mapping = sfnt(data), cmap(path)
    unit = size / struct.unpack_from('>H', data, tables[b'head'][0]+18)[0]
    def u16(p): return struct.unpack_from('>H', data, p)[0]
    def coverage(p):
        fmt, n = u16(p), u16(p+2)
        if fmt == 1:
            return list(struct.unpack_from(f'>{n}H', data, p+4))
        if fmt == 2:
            result = []
            for i in range(n):
                lo, hi, index = struct.unpack_from('>3H', data, p+4+i*6)
                if index != len(result): raise ValueError('GPOS coverage index')
                result.extend(range(lo, hi+1))
            return result
        raise ValueError('GPOS coverage format')
    def classes(p):
        fmt = u16(p)
        if fmt == 1:
            start, n = u16(p+2), u16(p+4)
            return dict(zip(range(start, start+n), struct.unpack_from(f'>{n}H', data, p+6)))
        if fmt == 2:
            result = {}
            for i in range(u16(p+2)):
                lo, hi, cls = struct.unpack_from('>3H', data, p+4+i*6)
                result.update(dict.fromkeys(range(lo, hi+1), cls))
            return result
        raise ValueError('GPOS class format')
    subtables, legacy = [], {}
    if b'GPOS' in tables:
        base = tables[b'GPOS'][0]
        feature, lookups = base+u16(base+6), base+u16(base+8)
        selected = set()
        for i in range(u16(feature)):
            p = feature+2+i*6
            if data[p:p+4] == b'kern':
                f = feature+u16(p+4)
                selected.update(struct.unpack_from(f'>{u16(f+2)}H', data, f+4))
        for i in sorted(selected):
            lookup = lookups+u16(lookups+2+i*2)
            if u16(lookup) != 2 or u16(lookup+2) != 0:
                raise ValueError('unsupported GPOS kern lookup/flags')
            group = []
            for j in range(u16(lookup+4)):
                p = lookup+u16(lookup+6+j*2)
                fmt = u16(p)
                if (u16(p+4), u16(p+6)) != (4, 0):
                    raise ValueError('unsupported GPOS kerning values')
                cov = coverage(p+u16(p+2))
                if fmt == 1:
                    pairs = {}
                    if len(cov) != u16(p+8): raise ValueError('GPOS pair count')
                    for n, left in enumerate(cov):
                        q = p+u16(p+10+n*2)
                        for k in range(u16(q)):
                            right, value = struct.unpack_from('>Hh', data, q+2+k*4)
                            pairs[left,right] = value
                    group.append(lambda l,r,pairs=pairs: pairs.get((l,r)))
                elif fmt == 2:
                    c1, c2 = classes(p+u16(p+8)), classes(p+u16(p+10))
                    rows, cols = u16(p+12), u16(p+14)
                    values = struct.unpack_from(f'>{rows*cols}h', data, p+16)
                    covered = set(cov)
                    group.append(lambda l,r,a=c1,b=c2,v=values,c=cols,cov=covered:
                                 v[a.get(l,0)*c+b.get(r,0)] if l in cov else None)
                else:
                    raise ValueError('unsupported GPOS pair format')
            subtables.append(group)
    elif b'kern' in tables:
        p = tables[b'kern'][0]
        if u16(p) != 0: raise ValueError('unsupported kern version')
        count, p = u16(p+2), p+4
        for _ in range(count):
            length, flags = u16(p+2), u16(p+4)
            if flags != 1: raise ValueError('unsupported kern format/flags')
            for i in range(u16(p+6)):
                left, right, value = struct.unpack_from('>2Hh', data, p+14+i*6)
                legacy[left,right] = legacy.get((left,right),0)+value
            p += length
    result = []
    for left in cps:
        for right in cps:
            l, r = mapping[left], mapping[right]
            value = legacy.get((l,r), 0)
            for group in subtables:
                for query in group:
                    adjustment = query(l,r)
                    if adjustment is not None:
                        value += adjustment
                        break
            if value: result.append((left, right, value*unit))
    return result


def hpt4(indices, width=PAGE, height=PAGE):
    if width % 32 or height % 8 or len(indices) != width*height or any(v > 15 for v in indices):
        raise ValueError('CLUT4 dimensions/indices')
    packed = bytes(indices[i] | indices[i+1] << 4 for i in range(0, len(indices), 2))
    return (b'HPT1' + struct.pack('<HHBBBBI', width, height, 4, 3, 1, 0, 16) +
            bytes(v for a in range(16) for v in (255, 255, 255, a*17)) +
            t.swizzle(packed, width//2, height))


def read_hpt4(data):
    if len(data) < 80 or data[:4] != b'HPT1':
        raise ValueError('HPT1 header')
    w, h, psm, clut, swiz, reserved, entries = struct.unpack_from('<HHBBBBI', data, 4)
    if (psm, clut, swiz, reserved, entries) != (4, 3, 1, 0, 16) or not w or not h or w%32 or h%8:
        raise ValueError('HPT1 CLUT4 header')
    if len(data) != 80+w*h//2:
        raise ValueError('HPT1 exact size')
    raw = t.unswizzle(data[80:], w//2, h)
    return w, h, bytes(v for b in raw for v in (b&15, b>>4))


def read_pft(data):
    if len(data) < 28 or data[:4] != b'PFT1':
        raise ValueError('PFT1 header')
    version, page, pages, nf, ng, nk = struct.unpack_from('<6I', data, 4)
    if version != 1 or page != PAGE or not 0 < pages <= 32 or nf != 4 or not 0 < ng <= 4096 or nk > 65536:
        raise ValueError('PFT1 version/counts')
    if len(data) != 28+nf*32+ng*40+nk*16:
        raise ValueError('PFT1 exact size')
    fonts = [struct.unpack_from('<4I4f', data, 28+i*32) for i in range(nf)]
    glyphs = [struct.unpack_from('<7I3f', data, 28+nf*32+i*40) for i in range(ng)]
    kerns = [struct.unpack_from('<3If', data, 28+nf*32+ng*40+i*16) for i in range(nk)]
    keys = {(g[1], g[0]) for g in glyphs}
    if len(keys) != ng:
        raise ValueError('PFT1 duplicate glyph')
    first = 0
    for i, f in enumerate(fonts):
        if (f[:4] != (i, SIZES[i][1], first, f[3]) or not f[3] or first+f[3] > ng or
                not all(math.isfinite(v) and v >= 0 for v in f[4:]) or
                f[6] < (11 if i >= 2 else 9) or f[7] < f[6] or
                any(g[1] != i for g in glyphs[first:first+f[3]])):
            raise ValueError('PFT1 font range/metrics/floor')
        first += f[3]
    if first != ng:
        raise ValueError('PFT1 glyph ranges')
    for cp, font, p, x, y, w, h, advance, bx, by in glyphs:
        if (cp > 0x10ffff or 0xd800 <= cp <= 0xdfff or font >= nf or p >= pages or
                not w or not h or x+w > page or y+h > page or
                not all(map(math.isfinite, (advance, bx, by))) or advance < 0):
            raise ValueError('PFT1 glyph')
    if len({k[:3] for k in kerns}) != nk:
        raise ValueError('PFT1 duplicate kerning')
    for font, left, right, value in kerns:
        if (font, left) not in keys or (font, right) not in keys or not math.isfinite(value) or value == 0:
            raise ValueError('PFT1 kerning')
    return fonts, glyphs, kerns


def convert(root, out):
    Image = t.import_pil()
    from PIL import ImageFont, ImageDraw
    cps, sources = charset(root)
    ru_cps, ru_sources = russian_charset(root)
    images, metrics, kerns, records, cyrillic, fallbacks = [], [], [], [], {}, {}
    for fid, (face, size) in enumerate(SIZES):
        path, ru_path = root/FACES[face], root/RU_FACES[face]
        en_map, ru_map = set(cmap(path)), set(cmap(ru_path))
        missing = set(cps)-en_map
        if missing:
            raise ValueError(f'{face} missing glyphs: {sorted(missing)}')
        # Fallback order: English face first, then the RU face. A code point neither face
        # has is not rendered; it is listed in the manifest instead of becoming .notdef.
        latin = set(cps)
        rest = [cp for cp in ru_cps if cp not in latin]
        extra = [(cp, 'en') for cp in rest if cp in en_map]
        extra += [(cp, 'ru') for cp in rest if cp not in en_map and cp in ru_map]
        fallbacks[face] = dict(en=[cp for cp, s in extra if s == 'en'],
                               ru=[cp for cp, s in extra if s == 'ru'],
                               uncovered=[cp for cp in rest if cp not in en_map and cp not in ru_map])
        entries = [(cp, 'en') for cp in cps] + extra
        # Hint each glyph at the accepted final size; pair positioning comes from
        # the source tables below, independently of optional Pillow RAQM support.
        fonts = {'en': ImageFont.truetype(str(path), size, layout_engine=ImageFont.Layout.BASIC),
                 'ru': ImageFont.truetype(str(ru_path), size, layout_engine=ImageFont.Layout.BASIC)}
        font = fonts['en']
        first = len(images)
        for cp, source in entries:
            ch = chr(cp)
            f_ = fonts[source]
            l, top, r, b = f_.getbbox(ch, anchor='ls')
            img = Image.new('L', (max(1, r-l), max(1, b-top)))
            ImageDraw.Draw(img).text((-l, -top), ch, font=f_, fill=255, anchor='ls')
            images.append(img.point(lambda v: (v*15+127)//255))
            metrics.append((cp, fid, f_.getlength(ch), l, top, source))
        ascent, descent = font.getmetrics()
        caps = font.getbbox('H', anchor='ls')
        records.append((fid, size, first, len(entries), ascent, descent, caps[3]-caps[1], ascent+descent))
        # Cyrillic capitals from the RU face must meet the same floors as the Latin ones.
        cyrillic[fid] = {ch: fonts['ru'].getbbox(ch, anchor='ls') for ch in 'ЖН'}
        cyrillic[fid] = {ch: box[3]-box[1] for ch, box in cyrillic[fid].items()}
        if min(cyrillic[fid].values()) < (11 if fid >= 2 else 9):
            raise ValueError(f'{face} {size}: Cyrillic capitals below floor {cyrillic[fid]}')
        en_cps = [cp for cp, s in entries if s == 'en']
        ru_only = [cp for cp, s in entries if s == 'ru']
        kerns.extend((fid, left, right, value) for left,right,value in kerning(path, en_cps, size))
        kerns.extend((fid, left, right, value) for left,right,value in kerning(ru_path, ru_only, size))
    places, count = t.pack([im.size for im in images], page=PAGE)
    pages = [Image.new('L', (PAGE, PAGE)) for _ in range(count)]
    glyphs = []
    for im, m, (p, x, y) in zip(images, metrics, places):
        pages[p].paste(im, (x, y))
        cp, fid, advance, bx, by, _ = m
        glyphs.append((cp, fid, p, x, y, *im.size, advance, bx, by))
    out.mkdir(parents=True, exist_ok=True)
    for i, im in enumerate(pages):
        data = hpt4(im.tobytes())
        read_hpt4(data)
        (out/f'page-{i}.hpt').write_bytes(data)
        im.point(lambda v: v*17).save(out/f'page-{i}.png')
    data = b'PFT1'+struct.pack('<6I', 1, PAGE, count, 4, len(glyphs), len(kerns))
    data += b''.join(struct.pack('<4I4f', *r) for r in records)
    data += b''.join(struct.pack('<7I3f', *g) for g in glyphs)
    data += b''.join(struct.pack('<3If', *k) for k in kerns)
    read_pft(data)
    (out/'fonts.bin').write_bytes(data)
    sources_by_font = {fid: {f'U+{cp:04X}': s for cp, f, _, _, _, s in metrics if f == fid}
                       for fid in range(len(SIZES))}
    result = dict(format='PFT1', version=1, pages=count, glyphs=len(glyphs), kerning_pairs=len(kerns),
                  codepoints=cps, latin1_letters=[f'U+{cp:04X}' for cp in cps if cp > 127],
                  fonts=[dict(face=face, size_px=size, cap_px=r[6], glyph_sources=sources_by_font[fid],
                              cyrillic_caps_px=cyrillic[fid], russian_fallback=fallbacks[face])
                         for fid, ((face, size), r) in enumerate(zip(SIZES, records))],
                  texture_bytes=count*(PAGE*PAGE//2+64), table_bytes=len(data),
                  source_sha256={str(p.relative_to(root)):t.sha256(p) for p in
                      sources+ru_sources+[root/FACES[face] for face in FACES]+[root/RU_FACES[face] for face in RU_FACES]})
    (out/'manifest.json').write_text(json.dumps(result, indent=1), encoding='utf-8')
    preview_russian(Image, out, root)
    if (t.REPO/'_private/converted-psp/hud/hud-visual.psp.bin').exists():
        preview(Image, out)
    return result


def preview_russian(Image, out, root):
    """One 480x272 frame: original Russian Tutorial_01 prompts and titles at each accepted size."""
    board = Image.new('RGB', (480, 272), (12, 16, 24))
    _, glyphs, kerns = read_pft((out/'fonts.bin').read_bytes())
    count = struct.unpack_from('<I', (out/'fonts.bin').read_bytes(), 12)[0]
    atlas = [Image.open(out/f'page-{i}.png') for i in range(count)]
    lookup = {(g[1], g[0]): g for g in glyphs}
    kerning = {k[:3]: k[3] for k in kerns}
    rows = (
        (3, 18, ['KINGSPASS_MAIN']),
        (2, 14, ['PROMPT_FOCUS']),
        (1, 11, ['PROMPT_ATTACK', 'PROMPT_JUMP', 'PROMPT_INTERACT']),
        (0, 15, ['PROMPT_FOCUS_EXPLAINER1A', 'PROMPT_FOCUS_EXPLAINER2']),
    )
    y = 30
    for fid, size, keys in rows:
        for key in keys:
            sheet = 'Titles' if key.startswith('KINGSPASS') else 'Prompts'
            for line in ru_text(root, sheet, key):
                x, previous = 12., None
                for ch in line:
                    cp, font, p, sx, sy, w, h, advance, bx, by = lookup[fid, ord(ch)]
                    x += kerning.get((fid, previous, cp), 0)
                    alpha = atlas[p].crop((sx, sy, sx+w, sy+h))
                    board.paste((240, 240, 240), (round(x+bx), round(y+by), round(x+bx)+w, round(y+by)+h), alpha)
                    x += advance
                    previous = cp
                y += size+6
    target = t.REPO/'build/psp/russian-fonts-preview.png'
    target.parent.mkdir(parents=True, exist_ok=True)
    board.save(target)


def preview(Image, out):
    """One 480x272 board: decoded final atlases, including a 1:1 HUD sprite strip."""
    board = Image.new('RGB', (480, 272), (12, 16, 24))
    from transcode_hud import read_psa
    hud = t.REPO/'_private/converted-psp/hud'
    bank = read_psa((hud/'hud-visual.psp.bin').read_bytes())
    x = 12
    for name in (b'HUD Frame Idle', b'Health Idle', b'Blue Idle', b'Coin Idle', b'Soul Eyes'):
        s = bank.sprites[bank.clips[name][0]]
        p, sx, sy, w, h = s[:5]
        im = Image.open(hud/f'page-{p}.png').crop((sx, sy, sx+w, sy+h)).convert('RGBA')
        board.paste(im, (x, 10), im)
        x += w+12
    _, glyphs, kerns = read_pft((out/'fonts.bin').read_bytes())
    count = struct.unpack_from('<I', (out/'fonts.bin').read_bytes(), 12)[0]
    atlas = [Image.open(out/f'page-{i}.png') for i in range(count)]
    lookup = {(g[1], g[0]):g for g in glyphs}
    kerning = {k[:3]:k[3] for k in kerns}
    for fid, y in enumerate((90, 130, 171, 219)):
        sample = ('Collect SOUL by striking enemies.', 'GEO 0123456789  Continue',
                  "HALLOWNEST  King's Pass", 'HOLLOW KNIGHT')[fid]
        x, previous = 12., None
        for ch in sample:
            cp, font, p, sx, sy, w, h, advance, bx, by = lookup[fid, ord(ch)]
            x += kerning.get((fid, previous, cp), 0)
            alpha = atlas[p].crop((sx, sy, sx+w, sy+h))
            board.paste((240, 240, 240), (round(x+bx), round(y+by), round(x+bx)+w, round(y+by)+h), alpha)
            x += advance
            previous = cp
    target = t.REPO/'build/psp/hud-assets-preview.png'
    target.parent.mkdir(parents=True, exist_ok=True)
    board.save(target)


if __name__ == '__main__':
    result = convert(t.source_root(), t.REPO/'_private/converted-psp/fonts')
    print(f"Fonts: {result['glyphs']} glyphs, {result['kerning_pairs']} pairs, {result['texture_bytes']} texture bytes")
