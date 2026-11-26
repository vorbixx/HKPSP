"""Append missing original pickup poses to the freshly transcoded PSP hero atlas.

Existing sprites, page numbers and sidecar indices remain unchanged. Regeneration
is idempotent: clips already supplied by the hero converter are never duplicated.
"""
import json
import math
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'tools/psp'))
import ability_source
import transcode_room as t
from transcode_hud import read_psa


def convert(snapshot=ability_source.SNAPSHOT, out=ROOT/'_private/converted-psp/hero'):
    snapshot, out = Path(snapshot), Path(out)
    if not out.resolve().is_relative_to((ROOT/'_private/converted-psp').resolve()):
        raise ValueError('Hero augmentation must stay in private PSP outputs')
    if not (snapshot/'snapshot.json').exists():
        ability_source.extract(snapshot)
    data = json.loads((snapshot/'snapshot.json').read_text(encoding='utf-8'))
    path = out/'hero.psp.bin'
    raw = path.read_bytes()
    table = read_psa(raw)
    clips = [c for n, c in data['hero_clips'].items() if n.encode() not in table.clips]
    if not clips:
        return {'clips': 0, 'pages': 0, 'texture_bytes': 0}
    indices = sorted({i for c in clips for i in c['export_frames']})
    Image = t.import_pil()
    ppu = t.room_focal_length(t.PSP_WIDTH, t.PSP_HEIGHT)/t.HERO_DEPTH
    bitmaps, geometries = [], []
    for i in indices:
        s = data['hero_sprites'][i]
        b = s['bounds']
        im = Image.open(snapshot/s['file']).convert('RGBA')
        size = (math.ceil((b[2]-b[0])*ppu), math.ceil((b[3]-b[1])*ppu))
        bitmaps.append(im.resize(size, Image.Resampling.LANCZOS))
        geometries.append(b)
    places, count = t.pack([im.size for im in bitmaps])
    pages = [Image.new('RGBA', (t.PAGE, t.PAGE)) for _ in range(count)]
    sprites = list(table.sprites)
    mapping = {}
    for i, im, b, (page, x, y) in zip(indices, bitmaps, geometries, places):
        t.paste_padded(pages[page], im, x, y)
        mapping[i] = len(sprites)
        sprites.append((table.pages+page, x, y, *im.size, *b))
    for i, page in enumerate(pages):
        quant = t.quantize_page(Image, page)
        number = table.pages+i
        (out/f'page-{number}.hpt').write_bytes(t.hpt1(t.PAGE, t.PAGE, bytes(quant.getpalette('RGBA')), quant.tobytes()))
        quant.convert('RGBA').save(out/f'page-{number}.png')
    frames = list(table.frames)
    clip_bytes = table.clip_bytes
    for c in clips:
        first = len(frames)
        frames.extend(mapping[i] for i in c['export_frames'])
        clip_bytes += struct.pack('<48sf4I', c['name'].encode(), c['fps'], c['wrapMode'], c['loopStart'], first, len(frames)-first)
    output = b'PSA1'+struct.pack('<6I', 1, t.PAGE, table.pages+count, len(sprites), table.n_clips+len(clips), len(frames))
    output += b''.join(struct.pack('<5I4f', *s) for s in sprites)+clip_bytes+struct.pack(f'<{len(frames)}I', *frames)
    read_psa(output)
    path.write_bytes(output)
    result = {'clips': len(clips), 'sprites': len(indices), 'pages': count,
              'texture_bytes': count*(t.PAGE*t.PAGE+1024), 'source_build': data['build']}
    (out/'ability-poses.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    return result


if __name__ == '__main__':
    print('Ability hero poses:', convert())
