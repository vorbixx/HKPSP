"""Repack source Watcher rectangles without resampling or changing geometry."""
import json
from pathlib import Path
import shutil
import struct
import subprocess
from PIL import Image


def place_rectangles(sizes, side=1024):
    """Deterministic best-short-side packing; rectangles never rotate."""
    free = []
    placements = {}
    for index in sorted(range(len(sizes)), key=lambda i: (-max(sizes[i]), -sizes[i][0]*sizes[i][1], i)):
        width, height = sizes[index]
        width += 2
        height += 2
        if max(width, height) > side:
            raise ValueError('Source sprite exceeds atlas size')
        choices = [(min(w-width,h-height), max(w-width,h-height), page,y,x)
                   for page,rects in enumerate(free) for x,y,w,h in rects if w>=width and h>=height]
        if choices:
            _,_,page,y,x = min(choices)
        else:
            page,x,y = len(free),0,0
            free.append([(0,0,side,side)])
        placements[index] = (page,x+1,y+1)
        split = []
        for rx,ry,rw,rh in free[page]:
            if x>=rx+rw or x+width<=rx or y>=ry+rh or y+height<=ry:
                split.append((rx,ry,rw,rh))
                continue
            if x>rx: split.append((rx,ry,x-rx,rh))
            if x+width<rx+rw: split.append((x+width,ry,rx+rw-x-width,rh))
            if y>ry: split.append((rx,ry,rw,y-ry))
            if y+height<ry+rh: split.append((rx,y+height,rw,ry+rh-y-height))
        split = sorted(set(split))
        free[page] = [r for r in split if not any(r!=s and r[0]>=s[0] and r[1]>=s[1]
                      and r[0]+r[2]<=s[0]+s[2] and r[1]+r[3]<=s[1]+s[3] for s in split)]
    return placements,len(free)


def repack(snapshot, output, tex3ds, stem='watcher'):
    snapshot,output = Path(snapshot),Path(output)
    data = (snapshot/f'{stem}-art.bin').read_bytes()
    magic,page_count,count,_,_ = struct.unpack_from('<4s4I',data)
    if magic!=b'HKA1': raise ValueError('Not a source animation bank')
    sources = [Image.open(snapshot/f'{stem}-page-{i}.png').convert('RGBA') for i in range(page_count)]
    records = [struct.unpack_from('<5I4f',data,20+i*36) for i in range(count)]
    for index,(page,x,y,width,height,*_) in enumerate(records):
        if (page >= len(sources) or width == 0 or height == 0 or
                x + width > sources[page].width or y + height > sources[page].height):
            raise ValueError(f'Sprite {index} rectangle exceeds its source page')
    placements,pages = place_rectangles([(r[3],r[4]) for r in records])
    images = [Image.new('RGBA',(1024,1024)) for _ in range(pages)]
    bank = bytearray(data)
    struct.pack_into('<I',bank,4,pages)
    extents = [(0,0)]*pages
    for i,(old,x,y,w,h,*_) in enumerate(records):
        page,px,py = placements[i]
        images[page].paste(sources[old].crop((x,y,x+w,y+h)),(px,py))
        # Leave authored world-space bounds and every clip/frame byte untouched.
        struct.pack_into('<3I',bank,20+i*36,page,px,py)
        ex,ey = extents[page]
        extents[page] = (max(ex,px+w+1),max(ey,py+h+1))
    romfs=output/'romfs'
    romfs.mkdir(parents=True,exist_ok=True)
    for stale in list(output.glob(f'{stem}-page-*.png'))+list(romfs.glob(f'{stem}-page-*.t3x')):
        stale.unlink()
    (romfs/f'{stem}-art.bin').write_bytes(bank)
    parts_path = snapshot / f'{stem}-parts.bin'
    packed_parts_path = romfs / f'{stem}-parts.bin'
    if parts_path.exists():
        shutil.copy2(parts_path, packed_parts_path)
    elif packed_parts_path.exists():
        packed_parts_path.unlink()
    dimensions=[]
    for i,image in enumerate(images):
        w,h=(max(8,1<<(n-1).bit_length()) for n in extents[i])
        dimensions.append((w,h))
        png=output/f'{stem}-page-{i}.png'
        image.crop((0,0,w,h)).save(png)
        subprocess.run([str(tex3ds),'-f','rgba4','-z','lz11','-o',str(romfs/f'{stem}-page-{i}.t3x'),str(png)],
                       check=True,capture_output=True)
    manifest=json.loads((snapshot/'manifest.json').read_text())
    manifest.update(pages=pages,texture_bytes=sum(w*h*2 for w,h in dimensions),page_dimensions=dimensions,
                    packing='best short side; unchanged source pixels, scale 1 and world geometry')
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2))
    print(json.dumps({k:manifest[k] for k in ('pages','sprites','texture_bytes')}))
