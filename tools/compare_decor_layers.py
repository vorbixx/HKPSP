"""Owner-attributed same-frame layer diagnostics, never a visual acceptance gate.

Visibility is measured with a plain camera at the original projection, using only
the observed owner renderers. A repeat empty render measures instability.
No tolerance is fitted to the complete before/after frame difference.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw
from compare_decor_host import KINDS, load, project
from compare_decor_particles import distribution


def read_rows(path):
    return [json.loads(line) for line in Path(path).read_text().splitlines()]


def validate_observations(directory, frames, owner, target, frame_rows):
    directory = Path(directory)
    objects = read_rows(directory / 'effect-objects.jsonl')
    particles = read_rows(directory / 'effect-particles.jsonl')
    timeline = {int(r['offset']): int(r['unityFrame']) for r in frame_rows}
    identities = {}
    seen = set()
    for row in objects:
        identity = row['instance']
        if row['owner'] != owner or row['target'] != target or row['offset'] not in frames:
            raise ValueError('Effect owner/frame mismatch')
        if row['unityFrame'] != timeline[row['offset']]:
            raise ValueError('Effect frame drift')
        if (row['offset'], identity) in seen:
            raise ValueError('Duplicate effect root')
        seen.add((row['offset'], identity))
        signature = (row['kind'], row['path'])
        if identities.setdefault(identity, signature) != signature:
            raise ValueError('Effect root identity changed')
    if not identities or seen != {(f, i) for f in frames for i in identities}:
        raise ValueError('Missing effect root offsets')
    kinds = [signature[0] for signature in identities.values()]
    if kinds.count('dust') != 1 or kinds.count('flash-strike') != 1 or kinds.count('flash-nail') != 1 or kinds.count('debris') != 2:
        raise ValueError('Unexpected owner effect roots')
    systems = {}
    for row in objects:
        for system in row['systems']:
            key = (row['offset'], system)
            if key in systems:
                raise ValueError('Particle system belongs to multiple effect roots')
            systems[key] = row['kind']
    particle_keys = set()
    def finite(value):
        if isinstance(value, float) and not math.isfinite(value):
            raise ValueError('Nonfinite effect state')
        if isinstance(value, dict):
            for item in value.values():
                finite(item)
        if isinstance(value, list):
            for item in value:
                finite(item)
    for row in particles:
        finite(row)
        key = (row['offset'], row['instance'])
        if key in particle_keys or systems.get(key) != row['kind']:
            raise ValueError('Unattributed/duplicate effect particle system')
        particle_keys.add(key)
        if row['owner'] != owner or row['target'] != target or row['unityFrame'] != timeline[row['offset']]:
            raise ValueError('Particle owner/frame mismatch')
        if row['count'] != len(row['particles']):
            raise ValueError('Effect particle count mismatch')
    if particle_keys != set(systems):
        raise ValueError('Missing effect particle system')
    return objects, particles


def contribution(layer, baseline, repeat):
    """One RGB LSB plus the local repeat-render error is the detection floor.

    This floor separates measured signal from render instability; it is not a
    permissible port/reference error. Report raw mass and repeat error as well.
    """
    delta = ImageChops.difference(layer, baseline).convert('RGB')
    noise = ImageChops.difference(repeat, baseline).convert('RGB')
    signal = [max(c) for c in delta.getdata()]
    errors = [max(c) for c in noise.getdata()]
    mask = Image.new('L', layer.size)
    mask.putdata([255 if value > error + 1 else 0 for value, error in zip(signal, errors)])
    weights = [value if mark else 0 for value, mark in zip(signal, mask.getdata())]
    mass = sum(weights)
    w = layer.width
    return dict(pixels=sum(bool(x) for x in weights), bbox=mask.getbbox(),
                mass=mass/255, raw_mass=sum(signal)/255,
                centroid=([sum((i % w)*v for i, v in enumerate(weights))/mass,
                           sum((i // w)*v for i, v in enumerate(weights))/mass] if mass else None),
                repeat_max=max(errors), repeat_mass=sum(errors)/255), delta


def compare(originals, fixtures, host, atlas, arm, output):
    output.mkdir(parents=True, exist_ok=False)
    host_rows = load(host)
    atlas_image = Image.open(atlas).convert('RGBA')
    report = dict(accepted=False, method='owner Spawn instances; isolated native sprite raster; repeat empty render',
                  detection_floor='local repeat error + 1 RGB LSB, not an acceptance tolerance',
                  limitations=['CPU projection is not ARM layer rendering',
                               'Different RNG streams prevent particle index matching',
                               'Isolated native sprite rasters omit post effects and terrain occlusion',
                               'Original RGB contribution mass and host alpha mass use different units'], rows=[], inputs={})
    def record(path):
        report['inputs'][str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
    for path in (host, atlas, fixtures, Path(__file__)):
        record(path)
    for fixture in json.loads(fixtures.read_text()):
        directory = originals / (fixture['name'] + '-layers-final')
        for path in directory.iterdir():
            if path.is_file():
                record(path)
        with (directory / 'probe-frames.csv').open() as stream:
            import csv
            frame_rows = list(csv.DictReader(stream))
        objects, particles = validate_observations(directory, fixture['frames'], fixture['owner'],
                                                   'Tute Pole ' + fixture['name'][-2:], frame_rows)
        sheet = Image.new('RGB', (400*8, 260*len(fixture['frames'])), '#15151a')
        draw = ImageDraw.Draw(sheet)
        for index, frame in enumerate(fixture['frames']):
            baseline = Image.open(directory / f'layer-{frame:03}-none-full.png').convert('RGB').resize((400, 240), Image.Resampling.LANCZOS)
            repeat = Image.open(directory / f'layer-{frame:03}-none-repeat-full.png').convert('RGB').resize((400, 240), Image.Resampling.LANCZOS)
            reference = Image.open(directory / f'frame-{frame:03}.png').convert('RGB')
            arm_frame = Image.open(arm / fixture['name'] / f"frame-{frame:03}.png").convert('RGB')
            record(arm / fixture['name'] / f"frame-{frame:03}.png")
            columns = [('original', reference), ('ARM HFX6', arm_frame)]
            projected = project([r for r in host_rows if int(r['owner']) == fixture['owner'] and int(r['frame']) == frame], atlas_image, fixture['camera'])
            for kind in KINDS:
                raster = Image.open(directory / f'layer-{frame:03}-{kind}-full.png').convert('RGB').resize((400, 240), Image.Resampling.LANCZOS)
                metrics, delta = contribution(raster, baseline, repeat)
                native = [r for r in particles if r['offset'] == frame and (r['kind'].startswith('flash') if kind == 'flash' else r['kind'] == kind)]
                selected = [r for r in host_rows if int(r['owner']) == fixture['owner'] and int(r['frame']) == frame and r['kind'] == kind]
                roots = [r for r in objects if r['offset'] == frame and (r['kind'].startswith('flash') if kind == 'flash' else r['kind'] == kind)]
                alpha = projected[kind].getchannel('A')
                row = dict(owner=fixture['owner'], offset=frame, kind=kind, original=metrics,
                           original_particle_count=sum(r['count'] for r in native),
                           original_systems=[dict(instance=r['instance'], count=r['count'], path=r['path']) for r in native],
                           original_active_renderers=sum(renderer['active'] and renderer['enabled'] for root in roots for renderer in root['renderers']),
                           original_particle_size=distribution([p['size'][0] for r in native for p in r['particles']]),
                           original_particle_color=[distribution([p['color'][c] for r in native for p in r['particles']]) for c in range(4)],
                           host_count=len(selected), host_alpha_mass=sum(alpha.getdata())/255,
                           host_bbox=alpha.point(lambda v: 255 if v > 1 else 0).getbbox())
                report['rows'].append(row)
                delta.save(output / f"{fixture['name']}-{frame:03}-{kind}-contribution.png")
                host_rgb = Image.alpha_composite(Image.new('RGBA', (400, 240), (0, 0, 0, 255)), projected[kind]).convert('RGB')
                columns += [(f'{kind} original contribution', delta), (f'{kind} host projection', host_rgb)]
            for col, (label, image) in enumerate(columns):
                sheet.paste(image, (col*400, index*260+20))
                draw.text((col*400+4, index*260+3), f'+{frame} {label}', fill='white')
        for kind in KINDS:
            if not any(row['original']['pixels'] for row in report['rows'] if row['owner'] == fixture['owner'] and row['kind'] == kind):
                raise ValueError('Unobservable layer: '+fixture['name']+'/'+kind)
        sheet.save(output / f"{fixture['name']}.png")
    (output / 'comparison.json').write_text(json.dumps(report, indent=2)+'\n')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--originals', type=Path, default=Path('_private/research/reference-frames'))
    parser.add_argument('--fixtures', type=Path, default=Path('tests/decor-frame-capture.json'))
    parser.add_argument('--host', type=Path, default=Path('build/decor-runtime-fixes/host-after.csv'))
    parser.add_argument('--atlas', type=Path, default=Path('_private/converted/decor-effects/decor-fx-page-0.png'))
    parser.add_argument('--arm', type=Path, default=Path('build/decor-runtime-fixes/arm-after'))
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    result = compare(args.originals, args.fixtures, args.host, args.atlas, args.arm, args.output)
    print(json.dumps(dict(rows=len(result['rows']), accepted=result['accepted'])))
