"""HKA1 HUD -> PSA1, preserving geometry, clips and frames. See the task brief for format.

HUD density is the measured original 52.694 px/unit at 1080p times .25.
Transforms already baked into sprite geometry must not be applied a second time.
"""
import json
import math
import re
import shutil
import struct
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import transcode_room as t
import area_title_art

PPU = 52.694 * .25
HUD_LARGE_PPU = PPU / .85


def header_values(root, filename, name):
    text = (root / 'include' / filename).read_text(encoding='utf-8')
    match = re.search(r'\b' + name + r'(?:\[\])?\s*=\s*([^;]+);', text)
    if not match:
        raise ValueError('missing HUD transform: ' + name)
    return [float(v) for v in re.findall(r'[-+]?\d+\.\d+(?:[eE][-+]?\d+)?', match[1])]


def density_factors(table, manifest, effects):
    densities = [None] * len(table.sprites)
    mask = manifest['layout']['relative']['mask'][2:4]
    liquid = manifest['liquid']['liquid_scale']
    for name, frames in table.clips.items():
        name = name.decode('utf-8')
        scale = mask if name.startswith(('Health', 'Blue')) else (1., 1.)
        # Original lore RectTransform scales (resources:9786, 9214, 10467).
        if name in ('Lore Arrow', 'Lore Stop'):
            scale = (1.3, 1.3)
        elif name in ('Lore Fleur Top', 'Lore Fleur Bottom'):
            scale = (.75, .75)
        elif name == 'Focus Panel':
            # Original level6:4395 under 5447 (Focus_prompt_temp).
            scale = (1.2067969*1.3119773, 1.2538149*1.262778)
        if name.startswith('Liquid '):
            scale = (liquid, liquid)
        if name in effects:
            scale = (effects[name],) * 2
        if len(scale) != 2 or not all(math.isfinite(v) and v > 0 for v in scale):
            raise ValueError('invalid HUD transform: ' + name)
        ppu = HUD_LARGE_PPU if name.startswith(('HUD Frame ', 'Soul ', 'Liquid ', 'Health ', 'Blue ', 'Coin ')) else PPU
        for i in frames:
            densities[i] = tuple(max(a, b * ppu) for a, b in zip(densities[i] or (0, 0), scale))
    if any(s is None for s in densities):
        raise ValueError('HUD sprite without a clip/transform')
    factors = []
    for s, density in zip(table.sprites, densities):
        factors.append((min(1., (s[7]-s[5])*density[0]/s[3]),
                        min(1., (s[8]-s[6])*density[1]/s[4])))
    return factors


def read_psa(data):
    if len(data) < 28 or data[:4] != b'PSA1':
        raise ValueError('PSA1 header')
    version, page, pages, sprites, clips, frames = struct.unpack_from('<6I', data, 4)
    if version != 1 or page != t.PAGE:
        raise ValueError('PSA1 version/page size')
    table = t.AnimationTable(b'HKA1' + struct.pack('<4I', pages, sprites, clips, frames) + data[28:])
    if any(s[1]+s[3] > page or s[2]+s[4] > page for s in table.sprites):
        raise ValueError('PSA1 rectangle')
    return table


def reminder_board(root, table):
    """Recover the original board before the inherited quarter-size reduction."""
    Image = t.import_pil()
    sys.path.insert(0, str(root / 'tools'))
    from convert_room_visuals import decode
    source = root / '_private/research/tutorial-reminders'
    mapping = json.loads((source / 'prompt-map.json').read_text(encoding='utf-8'))
    key = 'resources.assets-1803.json'
    texture = mapping['sprites'][key]
    for name in (key, texture):
        if t.sha256(source / name) != mapping['source_sha256'][name]:
            raise ValueError('Reminder source hash mismatch: ' + name)
    bitmap, geometry = decode(Image.open(source / texture).convert('RGBA'),
                              json.loads((source / key).read_text(encoding='utf-8')))
    # resources:9446. The original X transform exceeds the bitmap's resolution;
    # cap X to source texels, preserving the authored Y transform and open alpha.
    width = bitmap.width
    height = math.ceil((geometry[3]-geometry[1])*PPU*.61532986)
    if height > bitmap.height:
        raise ValueError('Reminder source resolution')
    bitmap = t.resample(Image, bitmap, (width, height))
    return {i: (bitmap, geometry, None) for i in table.clips[b'Prompt Backboard']}


def write_psa(path, pages, count, sprites, clips, frames):
    data = b'PSA1' + struct.pack('<6I', 1, t.PAGE, pages, count, len(clips)//68, len(frames))
    data += sprites + clips + struct.pack(f'<{len(frames)}I', *frames)
    read_psa(data)
    path.write_bytes(data)
    return len(data)


def convert_titles(root, out, package, table, factors, boards):
    baked, clips, frames = area_title_art.append(root, PPU, table, boards)
    first = len(table.sprites)
    with tempfile.TemporaryDirectory(dir=out) as temp:
        temp = Path(temp)
        _, _, _, sprites = t.build_pages(t.import_pil(), package, 'hud-page-{n}.png',
                                          table.pages, table.sprites, factors, temp, baked=baked)
        records = list(struct.iter_unpack('<9I', sprites))[first:]
        pages = sorted({record[0] for record in records})
        for index, page in enumerate(pages):
            shutil.copyfile(temp/f'page-{page}.hpt', out/f'region-title-page-{index}.hpt')
    sprites = b''.join(struct.pack('<9I', pages.index(record[0]), *record[1:]) for record in records)
    first_frame = len(table.frames)
    title_clips = bytearray(clips[len(table.clip_bytes):])
    for offset in range(60, len(title_clips), 68):
        struct.pack_into('<I', title_clips, offset,
                         struct.unpack_from('<I', title_clips, offset)[0]-first_frame)
    write_psa(out/'region-title.bin', len(pages), len(records), sprites, title_clips,
              [frame-first for frame in frames[first_frame:]])


def convert(root, out):
    package = root / '_private/converted/hud'
    source = package / 'hud-visual.bin'
    table = t.AnimationTable(source.read_bytes())
    manifest = json.loads((package / 'manifest.json').read_text(encoding='utf-8'))
    # Generated source curves are the exact piecewise-linear bounds used by hud.cpp.
    effects = {
        'Soul Limiter Fragment': max(header_values(root, 'hud_limiter_data.hpp', 'limiterSize')) *
            max(header_values(root, 'hud_limiter_data.hpp', 'limiterSizeMin') +
                header_values(root, 'hud_limiter_data.hpp', 'limiterSizeMax')),
        'Health Refill Particle': max(header_values(root, 'hud_liquid_data.hpp', 'refillSizeMin') +
                                      header_values(root, 'hud_liquid_data.hpp', 'refillSizeMax')) *
            max(header_values(root, 'hud_liquid_data.hpp', 'refillEmitters')[9::19]),
    }
    factors = density_factors(table, manifest, effects)
    boards = reminder_board(root, table)
    sizes, pages, stats, sprites = t.build_pages(t.import_pil(), package, 'hud-page-{n}.png',
                                                table.pages, table.sprites, factors, out,
                                                baked=boards)
    table_bytes = write_psa(out/'hud-visual.psp.bin', pages, len(sizes), sprites, table.clip_bytes, table.frames)
    convert_titles(root, out, package, table, factors, boards)
    result = dict(format='PSA1', version=1, pixels_per_unit=PPU, pages=pages,
                  sprites=len(sizes), clips=table.n_clips, frames=len(table.frames),
                  texture_bytes=pages*(t.PAGE*t.PAGE+1024), table_bytes=table_bytes,
                  factors=factors, sizes=sizes, effect_max_scales=effects, page_psnr=stats,
                  reminder_source_sha256={name:t.sha256(root/'_private/research/tutorial-reminders'/name)
                      for name in ('resources.assets-1803.json', 'resources.assets-203.png')},
                  source_sha256={p.name: t.sha256(p) for p in
                      [source, package/'manifest.json', *sorted(package.glob('hud-page-*.png'))]},
                  transform_sha256={name:t.sha256(root/'include'/name) for name in
                                    ('hud_liquid_data.hpp', 'hud_limiter_data.hpp')},
                  inherited_approximations=manifest['approximations'])
    (out / 'manifest.json').write_text(json.dumps(result, indent=1), encoding='utf-8')
    return result


if __name__ == '__main__':
    result = convert(t.source_root(), t.REPO/'_private/converted-psp/hud')
    print(f"HUD: {result['sprites']} sprites, {result['texture_bytes']} texture bytes")
