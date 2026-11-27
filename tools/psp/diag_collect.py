"""Lossless DIAG QOI collector; writes PNGs, numbered contact sheet and event summary."""
from __future__ import annotations

import argparse
import csv
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from transcode_room import import_pil


def decode_qoi(data: bytes):
    if len(data) < 22 or data[:4] != b'qoif':
        raise ValueError('Invalid QOI header')
    width, height, channels, colorspace = struct.unpack('>IIBB', data[4:14])
    if not width or not height or width * height > 480 * 272 or channels != 3 or colorspace != 0:
        raise ValueError('Invalid diagnostic QOI dimensions/format')
    index = [(0, 0, 0, 0)] * 64
    pixel = (0, 0, 0, 255)
    out = bytearray()
    pos, count = 14, 0
    end = len(data) - 8

    def take(n):
        nonlocal pos
        if pos + n > end:
            raise ValueError('Truncated QOI pixel stream')
        value = data[pos:pos+n]
        pos += n
        return value

    while count < width * height:
        tag = take(1)[0]
        run = 1
        r, g, b, a = pixel
        if tag == 0xfe:
            r, g, b = take(3)
        elif tag == 0xff:
            raise ValueError('Unexpected RGBA diagnostic QOI chunk')
        elif tag >> 6 == 0:
            r, g, b, a = index[tag]
        elif tag >> 6 == 1:
            r += ((tag >> 4) & 3) - 2
            g += ((tag >> 2) & 3) - 2
            b += (tag & 3) - 2
        elif tag >> 6 == 2:
            delta = (tag & 63) - 32
            second = take(1)[0]
            r += delta + (second >> 4) - 8
            g += delta
            b += delta + (second & 15) - 8
        else:
            run = (tag & 63) + 1
        pixel = (r & 255, g & 255, b & 255, a)
        if a != 255 or count + run > width * height:
            raise ValueError('Invalid QOI run/alpha')
        index[(pixel[0]*3 + pixel[1]*5 + pixel[2]*7 + a*11) % 64] = pixel
        out.extend(bytes(pixel[:3]) * run)
        count += run
    if pos != end or data[end:] != b'\0\0\0\0\0\0\0\1':
        raise ValueError('Invalid QOI end marker')
    return width, height, bytes(out)


def collect(folder: Path, destination: Path):
    if not (folder / 'complete.txt').is_file():
        raise ValueError(f'Incomplete diagnostic capture: {folder}')
    with (folder / 'diag.csv').open(newline='', encoding='ascii') as stream:
        rows = list(csv.DictReader(stream))
    if not rows or len(rows) > 90:
        raise ValueError('Expected 1-90 diagnostic frames')
    frames = [int(row['frame']) for row in rows]
    if any(a >= b for a, b in zip(frames, frames[1:])):
        raise ValueError('Diagnostic frames are not strictly increasing')
    Image = import_pil()
    from PIL import ImageDraw
    destination.mkdir(parents=True, exist_ok=True)
    columns, cell_w, cell_h = 6, 160, 110
    sheet = Image.new('RGB', (columns*cell_w, ((len(rows)+columns-1)//columns)*cell_h), '#202020')
    draw = ImageDraw.Draw(sheet)
    summary = ['# Diagnostic frames', '', f'{len(rows)} lossless RGB frames from `{folder}`.', '',
               'GE waits include the mandatory final synchronization (one per frame).',
               'Work/copy times are instrumentation, not PPSSPP performance acceptance.', '',
               '| Frame | Loads | Evictions | Pass evictions | GE waits | Events |',
               '|---:|---:|---:|---:|---:|---|']
    for i, row in enumerate(rows):
        frame = int(row['frame'])
        name = f'frame-{frame:08d}'
        encoded = (folder / (name+'.qoi')).read_bytes()
        if len(encoded) != int(row['encoded_bytes']):
            raise ValueError(f'Encoded size mismatch for frame {frame}')
        w, h, pixels = decode_qoi(encoded)
        image = Image.frombytes('RGB', (w, h), pixels)
        image.save(destination / (name+'.png'))
        x, y = (i % columns)*cell_w, (i // columns)*cell_h
        sheet.paste(image.resize((160, 91), Image.Resampling.NEAREST), (x, y))
        loads, evictions, passes, waits = [int(row[k]) for k in ('page_loads', 'evictions', 'pass_evictions', 'ge_waits')]
        label = f'{frame} L{loads} E{evictions} W{waits}'
        draw.text((x+2, y+93), label, fill='#ffb060' if loads or evictions or waits > 1 else 'white')
        summary.append(f'| {frame} | {loads} | {evictions} | {passes} | {waits} | {row["page_slot_events"]} |')
    sheet.save(destination / 'contact-sheet.png')
    (destination / 'summary.md').write_text('\n'.join(summary)+'\n', encoding='utf-8')
    return len(rows)


def captures(source: Path):
    if (source / 'diag.csv').is_file():
        return [source]
    # Accept capture roots, HKPSPDIAG folders, or USB drive roots.
    for candidate in (source, source/'diag', source/'PSP/GAME/HKPSPDIAG/diag'):
        found = sorted(p.parent for p in candidate.glob('*/diag.csv'))
        if found:
            return found
    raise ValueError(f'No diagnostic captures in {source}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    folders = captures(args.source)
    for folder in folders:
        target = args.output if len(folders) == 1 else args.output/folder.name
        print(f'{collect(folder, target)} frames -> {target}')
