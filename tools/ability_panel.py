"""Pack source item-panel sprites at the original single-screen quarter scale."""
import json
import math
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'tools/psp'))
import transcode_room as t
from transcode_hud import read_psa
import ability_source

# resources.assets Camera11812; original-player WorldToScreenPoint also confirms
# this projection at 1920x1080. Keep the PSP's two spare rows outside 480x270.
HUD_HALF_HEIGHT = 8.710663795471191
PPU = 270 / (2 * HUD_HALF_HEIGHT)
BURST_PPU = t.room_focal_length(480, 272)/(38.1-.881)
NAMES = ('cloak', 'claw', 'heart', 'key', 'fleur', 'stop', 'bg', 'trink_bg',
         'button-cross', 'button-l', 'button-r')


def convert(snapshot=ability_source.SNAPSHOT, out=ROOT/'_private/converted-psp/ability'):
    snapshot, out = Path(snapshot), Path(out)
    if not out.resolve().is_relative_to((ROOT/'_private/converted-psp').resolve()):
        raise ValueError('Ability atlas must stay in private PSP outputs')
    if not (snapshot/'snapshot.json').exists():
        ability_source.extract(snapshot)
    data = json.loads((snapshot/'snapshot.json').read_text(encoding='utf-8'))
    if 'button_skins' not in data or 'depth' not in data.get('pickup_burst', {}):
        # Existing local snapshots predate the dynamic controller-skin export.
        # Regeneration must upgrade them as well as a completely missing export.
        data = ability_source.extract(snapshot)
    if data['build'] != 22529139:
        raise ValueError('Unknown original build')
    cameras = data['hud_cameras']
    if (len(cameras) != 1 or cameras[0]['ref'] != ['resources.assets', 11812] or
            not cameras[0]['data']['orthographic'] or
            cameras[0]['data']['orthographic size'] != HUD_HALF_HEIGHT):
        raise ValueError('Unknown original pickup camera projection')
    burst = data['pickup_burst']
    if burst['ref'] != ['resources.assets', 1130] or burst['fps'] != 20 or burst['duration'] != .25:
        raise ValueError('Unknown original pickup burst')
    burst_names = tuple(f['sprite'] for f in burst['frames'])
    if len(burst_names) != 5 or any(abs(f['time']-i/20) > 1e-7 for i, f in enumerate(burst['frames'])):
        raise ValueError('Nonuniform original pickup burst')
    names = NAMES + tuple(f['sprite'] for f in data['panel_animations']['fleur']['frames']) + burst_names
    Image = t.import_pil()
    images, bounds = [], []
    for name in names:
        record = data['sprites'][name]
        b = list(record['bounds'])
        image = Image.open(snapshot/(name+'.png')).convert('RGBA')
        if list(image.size) != record['size']:
            raise ValueError('Sprite size differs from its source evidence: '+name)
        density = BURST_PPU if name in burst_names else PPU
        size = (math.ceil((b[2]-b[0])*density), math.ceil((b[3]-b[1])*density))
        if name in burst_names and (size[0] > image.width or size[1] > image.height):
            raise ValueError('Original source density is insufficient: '+name)
        image = image.resize(size, Image.Resampling.LANCZOS)
        if name == 'bg':
            # The original backdrop extends past the screen. Crop it at native
            # resolution rather than stretching a low-density black texture.
            x, y = (image.width-480)//2, (image.height-272)//2
            image = image.crop((x, y, x+480, y+272))
            b = [-240/PPU, -136/PPU, 240/PPU, 136/PPU]
        images.append(image)
        bounds.append(b)
    places, count = t.pack([im.size for im in images])
    pages = [Image.new('RGBA', (t.PAGE, t.PAGE)) for _ in range(count)]
    records = []
    for image, b, (page, x, y) in zip(images, bounds, places):
        t.paste_padded(pages[page], image, x, y)
        records.append(struct.pack('<5I4f', page, x, y, *image.size, *b))
    out.mkdir(parents=True, exist_ok=True)
    for i, page in enumerate(pages):
        quant = t.quantize_page(Image, page)
        palette = quant.getpalette('RGBA')
        (out/f'page-{i}.hpt').write_bytes(t.hpt1(t.PAGE, t.PAGE, bytes(palette), quant.tobytes()))
        quant.convert('RGBA').save(out/f'page-{i}.png')
    table = b'PSA1'+struct.pack('<6I', 1, t.PAGE, count, len(names), len(names)+1, len(names)+len(burst_names))
    table += b''.join(records)
    table += b''.join(struct.pack('<48sf4I', name.encode(), 1, 1, 0, i, 1) for i, name in enumerate(names))
    table += struct.pack('<48sf4I', b'Pickup Burst', burst['fps'], 2, 0, len(names), len(burst_names))
    frames = (*range(len(names)), *(names.index(n) for n in burst_names))
    table += struct.pack(f'<{len(frames)}I', *frames)
    read_psa(table)
    (out/'ability-panel.psp.bin').write_bytes(table)
    result = {'format': 'PSA1', 'pages': count, 'texture_bytes': count*(512*512+1024),
              'names': names, 'sizes': [im.size for im in images], 'bounds': bounds,
              'source_sha256': {p.name: t.sha256(p) for p in
                  [snapshot/'snapshot.json', *(snapshot/(n+'.png') for n in names)]}}
    (out/'manifest.json').write_text(json.dumps(result, indent=1), encoding='utf-8')
    return result


if __name__ == '__main__':
    d = convert()
    print(f"Ability panel: {len(d['names'])} sprites, {d['pages']} CLUT8 pages, {d['texture_bytes']} bytes")
