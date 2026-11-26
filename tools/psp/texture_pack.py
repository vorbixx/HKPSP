"""Write textures.pak: every staged HPT page in one file (PSP runtime opens it once).

Layout (little-endian): b"HTX1", u32 count, count x {char name[48], u32 offset, u32 size},
zero padding; each page's bytes start at a 2048-byte aligned offset. The PSP reader is
psp/src/page_residency.cpp TexturePack. Run 6 (2026-11-16): one sceIoOpen per page load
cost 13.5 ms on the PSP-3000.
"""
import pathlib
import struct
import sys

ALIGN = 2048
NAME = 48


def write_pack(directory: pathlib.Path) -> pathlib.Path:
    pages = sorted(p for p in directory.iterdir() if p.is_file() and p.suffix == '.hpt')
    if not pages:
        raise SystemExit(f'no .hpt files in {directory}')
    for page in pages:
        if len(page.name.encode('ascii')) >= NAME:
            raise SystemExit(f'texture name too long for the pack index: {page.name}')
    header = 8 + len(pages) * (NAME + 8)
    offset = (header + ALIGN - 1) // ALIGN * ALIGN
    index, blobs = [], []
    for page in pages:
        data = page.read_bytes()
        index.append(struct.pack('<48sII', page.name.encode('ascii'), offset, len(data)))
        blobs.append((offset, data))
        offset = (offset + len(data) + ALIGN - 1) // ALIGN * ALIGN
    out = directory / 'textures.pak'
    with open(out, 'wb') as f:
        f.write(b'HTX1' + struct.pack('<I', len(pages)) + b''.join(index))
        for start, data in blobs:
            f.write(b'\0' * (start - f.tell()))
            f.write(data)
    # Read back: every entry must equal its source file.
    pak = out.read_bytes()
    for entry, page in zip(index, pages):
        _, start, size = struct.unpack('<48sII', entry)
        if pak[start:start + size] != page.read_bytes():
            raise SystemExit(f'textures.pak mismatch for {page.name}')
    print(f'textures.pak: {len(pages)} pages, {len(pak)} bytes')
    return out


if __name__ == '__main__':
    write_pack(pathlib.Path(sys.argv[1]))
