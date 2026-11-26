"""Project runtime probe CSV with the exported atlas; diagnostic, not a GPU gate."""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw

KINDS = ('debris', 'dust', 'flash')


def load(path):
    rows = []
    with path.open(newline='') as source:
        for row in csv.DictReader(source):
            rows.append({k: v if k == 'kind' else float(v) for k, v in row.items()})
    return rows


def project(rows, atlas, camera):
    layers = {kind: Image.new('RGBA', (400, 240)) for kind in KINDS}
    focal = 240 / (2 * math.tan(math.radians(25.6) / 2))
    for p in rows:
        s = focal / max(10, 38.1 + p['z'])
        width, height = p['right']-p['left'], p['top']-p['bottom']
        a, b = s*p['a']*width/p['sw'], -s*p['b']*height/p['sh']
        d, e = -s*p['c']*width/p['sw'], s*p['d']*height/p['sh']
        c = 200+s*(p['a']*p['left']+p['b']*p['top']+p['tx']-camera[0])
        f = 120-s*(p['c']*p['left']+p['d']*p['top']+p['ty']-camera[1])
        det = a*e-b*d
        if abs(det) < 1e-10:
            continue
        sprite = atlas.crop(tuple(int(p[k]) for k in ('sx', 'sy')) +
                            (int(p['sx']+p['sw']), int(p['sy']+p['sh'])))
        channels = sprite.split()
        sprite = Image.merge('RGBA', tuple(ch.point([round(i*max(0, min(1, p[key])))
                    for i in range(256)]) for ch, key in zip(channels, ('r','g','blue','alpha'))))
        inverse = (e/det, -b/det, (b*f-e*c)/det,
                   -d/det, a/det, (d*c-a*f)/det)
        stamp = sprite.transform((400,240), Image.Transform.AFFINE, inverse,
                                 Image.Resampling.BILINEAR)
        layers[p['kind']] = Image.alpha_composite(layers[p['kind']], stamp)
    return layers


def metrics(layer, delta):
    mask = layer.getchannel('A').point(lambda x: 255 if x > 8 else 0)
    visible = list(mask.getdata())
    changes = list(delta.getdata())
    count = sum(bool(x) for x in visible)
    return dict(projected_pixels=count, bbox=mask.getbbox(),
                reference_delta_overlap=sum(bool(m) and max(c)>20 for m,c in zip(visible,changes)),
                # Whole/remnant changes, ambient, camera vibration and other effects
                # also change these reference pixels; overlap is not attribution.
                alpha_mass=sum(layer.getchannel('A').getdata())/255)


def compare(before, after, fixtures, originals, port, atlas_path, output):
    output.mkdir(parents=True, exist_ok=False)
    old, new, atlas = load(before), load(after), Image.open(atlas_path).convert('RGBA')
    report = dict(method='Host runtime 60 Hz control / authored fixed step, presentation poses, CPU atlas projection',
                  limitations=['CPU normal alpha blend omits GPU lighting/material/quantization and scene occlusion',
                               'Host RNG reset per pole; ARM second pole has a different random stream',
                               'Neutral host scheduler phase; saved ARM pre-hit phase may differ by a fixed tick',
                               'Unity end-of-frame emission differs from fixed-step dust births and immediate debris',
                               'Reference camera vibration up to about 0.1 world unit; host camera is fixed',
                               'Image delta overlap cannot identify debris/dust/flash in the reference',
                               'ARM capture provenance is supplied separately from this diagnostic'], inputs={}, frames=[])
    def record(path):
        report['inputs'][str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
    for path in (before, after, atlas_path):
        record(path)
    for fixture in fixtures:
        reference = originals/fixture['reference']
        record(reference/'probe-frames.csv')
        record(reference/'provenance.json')
        record(reference/'before.png')
        base = Image.open(reference/'before.png').convert('RGB')
        sheet = Image.new('RGB', (1600, len(fixture['frames'])*264), '#10141b')
        labels = ('Steam reference', 'Host BEFORE: FX only', 'Host AFTER: FX only', 'ARM capture')
        for row, frame in enumerate(fixture['frames']):
            images, all_layers = [], []
            ref_path = reference/f'frame-{frame:03}.png'
            arm_path = port/fixture['name']/f'frame-{frame:03}.png'
            record(ref_path); record(arm_path)
            original = Image.open(ref_path).convert('RGB')
            images.append(original)
            delta = ImageChops.difference(original, base)
            result = dict(name=fixture['name'], frame=frame)
            for tag, data in (('before',old), ('after',new)):
                particles = [p for p in data if p['owner']==fixture['owner'] and p['frame']==frame]
                layers = project(particles, atlas, fixture['camera'])
                all_layers.append(layers)
                for kind in KINDS:
                    layers[kind].save(output/f"{fixture['name']}-{frame:03}-{tag}-{kind}.png")
                fx = Image.new('RGBA', (400,240), '#10141b')
                for kind in KINDS:
                    fx = Image.alpha_composite(fx, layers[kind])
                images.append(fx.convert('RGB'))
                result[tag] = {kind:dict(count=sum(p['kind']==kind for p in particles),
                                         **metrics(layers[kind],delta)) for kind in KINDS}
            result['unchanged_layers'] = {kind:all_layers[0][kind].tobytes()==all_layers[1][kind].tobytes()
                                          for kind in KINDS}
            images.append(Image.open(arm_path).convert('RGB'))
            for column, (label, im) in enumerate(zip(labels, images)):
                sheet.paste(im, (column*400, row*264+24))
                ImageDraw.Draw(sheet).text((column*400+4,row*264+5), f"{fixture['name']} +{frame}: {label}", fill='white')
            report['frames'].append(result)
        sheet.resize((1200, len(fixture['frames'])*198)).save(output/(fixture['name']+'.png'))
    (output/'comparison.json').write_text(json.dumps(report, indent=2)+'\n')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', type=Path, default=Path('build/decor-host-before.csv'))
    parser.add_argument('--after', type=Path, default=Path('build/decor-host-after.csv'))
    parser.add_argument('--fixtures', type=Path, default=Path('tests/decor-frame-capture.json'))
    parser.add_argument('--originals', type=Path, default=Path('_private/research/reference-frames'))
    parser.add_argument('--port', type=Path, default=Path('build/emulator/automated/reference-capture'))
    parser.add_argument('--atlas', type=Path, default=Path('_private/converted/decor-effects/decor-fx-page-0.png'))
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    result = compare(args.before, args.after, json.loads(args.fixtures.read_text()), args.originals,
                     args.port, args.atlas, args.output)
    print(json.dumps(dict(frames=len(result['frames']), output=str(args.output))))
