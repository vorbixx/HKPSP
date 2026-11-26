"""Rasterize the original embedded HUD font with unhinted FreeType."""
import argparse
import base64
import csv
import ctypes as ct
import hashlib
import io
import json
import os
from pathlib import Path
import sys

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from extract_crawler import write_asset


FONT_SIZE = 45
LOAD_RENDER = 1 << 2
LOAD_NO_HINTING = 1 << 1
GRAYSCALE = 2
ALPHA_SHA256 = 'acfa5e71f3d47cafd53c1c69df0b99dfb20bbac575ac267fadd84dea37096783'
# Native TextMesh CharacterInfo bounds (left, right, bottom, top) and advances.
METRICS = (
    ('0', 0, 27, -2, 36, 27), ('1', 3, 24, -2, 36, 27),
    ('2', 0, 27, -1, 36, 27), ('3', 1, 26, -2, 36, 27),
    ('4', -1, 28, -2, 36, 27), ('5', 2, 25, -2, 36, 27),
    ('6', 0, 27, -2, 36, 27), ('7', 2, 27, -2, 36, 27),
    ('8', 0, 27, -2, 36, 27), ('9', 0, 26, -3, 36, 27),
    ('+', 0, 24, -1, 23, 25), ('-', 0, 19, 8, 16, 19),
    (' ', -1, 1, -1, 1, 14),
)


class Generic(ct.Structure):
    _fields_ = [('data', ct.c_void_p), ('finalizer', ct.c_void_p)]


class Bitmap(ct.Structure):
    _fields_ = [('rows', ct.c_uint), ('width', ct.c_uint), ('pitch', ct.c_int),
                ('buffer', ct.c_void_p), ('num_grays', ct.c_ushort),
                ('pixel_mode', ct.c_ubyte), ('palette_mode', ct.c_ubyte), ('palette', ct.c_void_p)]


class GlyphSlot(ct.Structure):
    _fields_ = [('library', ct.c_void_p), ('face', ct.c_void_p), ('next', ct.c_void_p),
                ('glyph_index', ct.c_uint), ('generic', Generic), ('metrics', ct.c_long * 8),
                ('linear_h_advance', ct.c_long), ('linear_v_advance', ct.c_long),
                ('advance', ct.c_long * 2), ('format', ct.c_uint32), ('bitmap', Bitmap),
                ('bitmap_left', ct.c_int), ('bitmap_top', ct.c_int)]


class Face(ct.Structure):
    _fields_ = [('counts_and_flags', ct.c_long * 5), ('family_name', ct.c_char_p),
                ('style_name', ct.c_char_p), ('num_fixed_sizes', ct.c_int), ('sizes', ct.c_void_p),
                ('num_charmaps', ct.c_int), ('charmaps', ct.c_void_p), ('generic', Generic),
                ('bbox', ct.c_long * 4), ('units_per_em', ct.c_ushort), ('heights', ct.c_short * 7),
                ('glyph', ct.POINTER(GlyphSlot))]


def load_freetype(path):
    # Windows: resolve the DLL's local dependencies (vcruntime) through the DLL loader.
    # Linux: the system libfreetype.so.6 finds its own dependencies.
    if hasattr(os, 'add_dll_directory'):
        with os.add_dll_directory(str(path.parent)):
            ft = ct.CDLL(str(path))
    else:
        ft = ct.CDLL(str(path))
    pointer = ct.c_void_p
    face = ct.POINTER(Face)
    signatures = {
        'Init_FreeType': (ct.POINTER(pointer),),
        'Done_FreeType': (pointer,),
        'New_Memory_Face': (pointer, ct.c_char_p, ct.c_long, ct.c_long, ct.POINTER(face)),
        'Done_Face': (face,),
        'Set_Pixel_Sizes': (face, ct.c_uint, ct.c_uint),
        'Load_Char': (face, ct.c_ulong, ct.c_int32),
    }
    for name, arguments in signatures.items():
        getattr(ft, 'FT_' + name).argtypes = arguments
    return ft


def check(code, operation):
    if code:
        raise ValueError(f'FreeType {operation} failed with error {code}')


def rasterize(dll, font):
    ft = load_freetype(dll)
    library = ct.c_void_p()
    check(ft.FT_Init_FreeType(ct.byref(library)), 'initialization')
    try:
        face = ct.POINTER(Face)()
        check(ft.FT_New_Memory_Face(library, font, len(font), 0, ct.byref(face)), 'font load')
        try:
            check(ft.FT_Set_Pixel_Sizes(face, 0, FONT_SIZE), 'pixel size')
            for char, left, right, bottom, top, advance in METRICS:
                check(ft.FT_Load_Char(face, ord(char), LOAD_RENDER | LOAD_NO_HINTING), f'glyph {char!r}')
                glyph = face.contents.glyph.contents
                if round(glyph.advance[0] / 64) != advance:
                    raise ValueError(f'Unexpected advance for HUD glyph {char!r}')
                canvas = Image.new('L', (right - left, top - bottom))
                if char != ' ':
                    bitmap = glyph.bitmap
                    if (bitmap.pixel_mode != GRAYSCALE or bitmap.pitch < bitmap.width
                            or not bitmap.buffer or bitmap.num_grays != 256):
                        raise ValueError(f'Expected an 8-bit FreeType bitmap for {char!r}')
                    alpha = Image.frombytes('L', (bitmap.width, bitmap.rows),
                        ct.string_at(bitmap.buffer, bitmap.pitch * bitmap.rows),
                        'raw', 'L', bitmap.pitch, 1)
                    canvas.paste(alpha, (glyph.bitmap_left - left, top - glyph.bitmap_top))
                yield char, canvas
        finally:
            ft.FT_Done_Face(face)
    finally:
        ft.FT_Done_FreeType(library)


def prepare(dll, research):
    hud = research / 'hud'
    source = hud / json.loads((hud / 'hud-map.json').read_text())['font']
    raw = source.read_bytes()
    font = json.loads(raw)
    layout = json.loads((research / 'hud-layout/layout-map.json').read_text())
    if font['m_Name'] != 'TrajanPro-Bold' or layout['font_size'] != FONT_SIZE:
        raise ValueError('Unexpected original Geo HUD typeface or size')
    output = research / 'hud-geo-font'
    if output.resolve() != output or any((output / name).is_symlink() for name in
                                         ('font-map.json', 'font-atlas.png', 'glyphs.csv')):
        raise ValueError(f'Geo font export requires a direct destination: {output}')
    glyphs = dict(rasterize(dll, base64.b64decode(font['m_FontData'])))
    alpha = b''.join(glyphs[char].tobytes() for char, *_ in METRICS if char != ' ')
    if hashlib.sha256(alpha).hexdigest() != ALPHA_SHA256:
        raise ValueError('FreeType raster differs from the reviewed Steam Geo glyphs')
    csv_bytes = io.StringIO(newline='')
    writer = csv.writer(csv_bytes, lineterminator='\n')
    writer.writerow(('code', 'advance', 'minX', 'maxX', 'minY', 'maxY',
                     'tlX', 'tlY', 'trX', 'trY', 'blX', 'blY', 'brX', 'brY'))
    # Four rows of four slots. Use a 64-pixel pitch vertically for the glyphs
    # whose authored bounds are taller than a 32-pixel slot.
    atlas = Image.new('RGBA', (128, 256), (255, 255, 255, 0))
    for index, (char, left, right, bottom, top, advance) in enumerate(METRICS):
        x, y = index % 4 * 32, index // 4 * 64
        mask = glyphs[char]
        tile = Image.new('RGBA', mask.size, (255, 255, 255, 0))
        tile.putalpha(mask)
        atlas.paste(tile, (x, y))
        u, v = x / atlas.width, 1 - y / atlas.height
        right_u, bottom_v = (x + mask.width) / atlas.width, 1 - (y + mask.height) / atlas.height
        writer.writerow((ord(char), advance, left, right, bottom, top,
                         u, v, right_u, v, u, bottom_v, right_u, bottom_v))
    png = io.BytesIO()
    atlas.save(png, format='PNG')
    files = {'font-atlas.png': png.getvalue(), 'glyphs.csv': csv_bytes.getvalue().encode()}
    manifest = dict(build_id=22529139, method='freetype-unhinted', font_size=FONT_SIZE,
                    character_size=1, transform_scale=layout['relative']['text'][2],
                    pixel_scale=font['m_PixelScale'], atlas='font-atlas.png', glyphs='glyphs.csv',
                    original_font_sha256=hashlib.sha256(raw).hexdigest(),
                    source_sha256={name: hashlib.sha256(value).hexdigest() for name, value in files.items()})
    files['font-map.json'] = (json.dumps(manifest, indent=2) + '\n').encode()
    for name, data in files.items():
        destination = output / name
        if not destination.is_file() or destination.read_bytes() != data:
            write_asset(destination, data)
    print('Geo font reproduced from the embedded Steam typeface')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--freetype', type=Path, required=True)
    parser.add_argument('--research', type=Path, required=True)
    args = parser.parse_args()
    prepare(args.freetype.resolve(strict=True), args.research.absolute())
