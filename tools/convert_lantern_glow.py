"""Export resources.assets Lantern Glow prefab 7964 as a bounded four-quad effect."""
import argparse
import bisect
import hashlib
import json
import math
from pathlib import Path
import shutil
import struct
import converted_files
from PIL import Image
import tex3ds_cache
from assetripper_client import ROOT
from tool_paths import TEX3DS as TEX3DS_EXE


def streamed_channels(clip):
    data = clip['m_MuscleClip']['m_Clip']['data']['m_StreamedClip']
    assert data['curveCount'] == 19
    raw = struct.pack('<%sI' % len(data['data']), *data['data'])
    channels = [[] for _ in range(19)]
    offset = 0
    while offset < len(raw):
        time, count = struct.unpack_from('<fi', raw, offset)
        offset += 8
        for _ in range(count):
            index, *coefficients = struct.unpack_from('<i4f', raw, offset)
            offset += 20
            if math.isfinite(time) and time >= 0:
                channels[index].append((time, coefficients))
    assert all(c and c[0][0] == 0 for c in channels)
    # Unity streamed coefficients are cubic polynomials relative to each key.
    for channel in channels:
        for (time, c), (next_time, next_c) in zip(channel, channel[1:]):
            dt = next_time - time
            assert abs(((c[0]*dt+c[1])*dt+c[2])*dt+c[3]-next_c[3]) < .001
    return channels


def sample(channel, time):
    index = bisect.bisect_right([key[0] for key in channel], time) - 1
    start, c = channel[max(0, index)]
    dt = time - start
    return ((c[0]*dt+c[1])*dt+c[2])*dt+c[3]


def convert(source, output, romfs, tex3ds):
    output.mkdir(parents=True, exist_ok=True)
    romfs.mkdir(parents=True, exist_ok=True)
    def read(identity):
        return json.loads((source/f'resources.assets-{identity}.json').read_text())
    # These SpriteRenderers use neutral Sprites-Default tint. Animated alpha is
    # sampled below; do not invent a warm RGB tint or bake the initial root
    # scale (.1): Darkness Control replaces that scale with an absolute tween.
    for renderer in (16418, 16561, 16464, 16571):
        assert read(renderer)['m_Color'] == dict(r=1, g=1, b=1, a=1)
    material = json.loads((source/'material.json').read_text())
    assert material['m_Name'] == 'Sprites-Default'
    assert all(color == dict(r=1, g=1, b=1, a=1)
               for _, color in material['m_SavedProperties']['m_Colors'])
    clip = read(1132)
    bindings = clip['m_ClipBindingConstant']['genericBindings']
    assert [(b['path'], b['attribute']) for b in bindings[:5]] == [
        (442686731, 4), (3142101868, 3), (3998730003, 3), (2517448786, 3), (442686731, 3)]
    duration = clip['m_MuscleClip']['m_StopTime']
    count = round(duration*60)+1
    assert count == 362
    channels = streamed_channels(clip)
    raw = bytearray(struct.pack('<4sIf', b'LNG1', count, duration))
    page = Image.new('RGBA', (512, 256))
    for identity, x in [(2602, 2), (1959, 130)]:
        meta = read(identity)
        image = Image.open(source/f'sprite-{identity}.png').convert('RGBA')
        w, h = round(meta['m_Rect']['width']), round(meta['m_Rect']['height'])
        if image.size != (w, h):
            # UnityPy exports the packed rect. Restore the authored sprite canvas;
            # approximation: fractional atlas offsets round to source pixels.
            offset = meta['m_RD']['textureRectOffset']
            canvas = Image.new('RGBA', (w, h))
            canvas.paste(image, (round(offset['x']), h-round(offset['y'])-image.height))
            image = canvas
        assert meta['m_Pivot'] == {'x': .5, 'y': .5}
        ppu = meta['m_PixelsToUnits']
        page.paste(image, (x, 2))
        raw += struct.pack('<4I2f', x, 2, w, h, w/ppu, h/ppu)
    # Orb, growing orb, shrinking orb, rotating beams. Bake original cubic
    # curves at 60 Hz; runtime interpolates these samples without allocation.
    for frame in range(count):
        time = min(duration, frame/60)
        values = []
        for scale, alpha, rotation in [(9, 15, None), (3, 16, None), (6, 17, None), (12, 18, 2)]:
            values.extend([sample(channels[scale], time), sample(channels[scale+1], time),
                           math.radians(sample(channels[rotation], time)) if rotation is not None else 0,
                           min(1, max(0, sample(channels[alpha], time)))])
        raw += struct.pack('<16f', *values)
    (output/'lantern-glow.bin').write_bytes(raw)
    page.save(output/'lantern-glow.png')
    tex3ds_cache.run([str(tex3ds), '-f', 'rgba8', '-z', 'none', '-o',
                     str(output/'lantern-glow.t3x'), str(output/'lantern-glow.png')], check=True)
    for name in ['lantern-glow.bin', 'lantern-glow.t3x']:
        shutil.copyfile(output/name, romfs/name)
    inputs = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(source.glob('*')) if p.is_file()}
    (output/'manifest.json').write_text(json.dumps(dict(prefab=7964, clip=1132, frames=count,
        duration=duration, atlas=[512,256], input_sha256=inputs), indent=2)+'\n')
    print('Lantern Glow:', count, 'frames,', len(raw), 'metadata bytes, 512 KiB texture')


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('--source', type=Path, default=ROOT/'_private/research/lantern-glow')
    p.add_argument('--output', type=Path, default=ROOT/'_private/converted/lantern-glow')
    p.add_argument('--romfs', type=Path, default=ROOT/'_private/converted/hero/romfs')
    p.add_argument('--tex3ds', type=Path, default=Path(TEX3DS_EXE))
    a = p.parse_args()
    convert(a.source, a.output, a.romfs, a.tex3ds)
