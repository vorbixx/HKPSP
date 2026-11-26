"""Offline fill-yield estimate for merging static fog/haze layers (reads banks only).

Layers can share one texture only if they have the same projection: identical depth,
layer/order, blend material, tint and sprite. The estimate counts the projected
target pixels the merged axis-aligned quad would cost against the member quads at
given cameras. It never modifies runtime assets.
"""
import argparse
import json
import math
import re
from pathlib import Path

from fog_composite import load_bank

ROOT = Path(__file__).resolve().parents[1]
BACKGROUND_DEPTH = 13.71  # roomBlurSplitDepth: deeper draws render into the blurred target
TARGETS = {'foreground': (400, 240), 'background': (600, 360)}
FOG_NAME = re.compile(r'^(fog|haze)', re.IGNORECASE)


def focal(width, height):
    """Mirror of roomFocalLength(): 24 degrees vertical, widened below 16:9."""
    aspect = max(1.6, width / height)
    fov = 24 * max(1, (16 / 9) / aspect)
    return height / (2 * math.tan(math.radians(fov) / 2))


def target_for(depth):
    return 'background' if depth >= BACKGROUND_DEPTH else 'foreground'


def corners(entry, sprite):
    """World corners and depths of one draw (matrix rows a,b,tx / c,d,ty / z,zx,zy)."""
    v = entry['values']
    return [(v[0]*x + v[1]*y + v[2], v[3]*x + v[4]*y + v[5], 38.1 + v[6] + v[13]*x + v[14]*y)
            for x in (sprite[5], sprite[7]) for y in (sprite[6], sprite[8])]


def projected_pixels(points, camera, target):
    """Pixels of the projected corner bounds inside the target (the renderer culls the same way)."""
    width, height = TARGETS[target]
    f = focal(width, height)
    xs = [width/2 + f*(x - camera[0])/max(10, z) for x, _, z in points]
    ys = [height/2 - f*(y - camera[1])/max(10, z) for _, y, z in points]
    return (max(0, min(max(xs), width) - max(min(xs), 0)) *
            max(0, min(max(ys), height) - max(min(ys), 0)))


def load_names(directory, prefix):
    cache = {}
    def name(owner):
        if owner not in cache:
            try:
                cache[owner] = json.loads((directory / f'{prefix}-{owner}.json').read_text())['m_Name']
            except (OSError, KeyError, ValueError):
                cache[owner] = ''
        return cache[owner]
    return name


def fog_layers(sprites, entries, name):
    layers = []
    for index, entry in enumerate(entries):
        if entry['values'][10] <= 0 or not FOG_NAME.match(name(entry['owner'])):
            continue
        v, sprite = entry['values'], sprites[entry['sprite']]
        pts = corners(entry, sprite)
        xs, ys = [p[0] for p in pts], [p[1] for p in pts]
        layers.append(dict(
            draw=index, owner=entry['owner'], name=name(entry['owner']), depth=v[6],
            key=(round(v[11]), v[12], round(v[15]), tuple(round(t, 3) for t in v[7:11]), entry['sprite']),
            box=(min(xs), min(ys), max(xs), max(ys)), points=pts,
            area=abs((v[0]*v[4] - v[1]*v[3]) * (sprite[7]-sprite[5]) * (sprite[8]-sprite[6])),
            flat=v[1] == 0 and v[3] == 0 and v[13] == 0 and v[14] == 0))
    return layers


def overlaps(a, b):
    return a[0] < b[2] and b[0] < a[2] and a[1] < b[3] and b[1] < a[3]


def merge_groups(layers, tolerance=1e-4):
    """Overlapping, flat, same-projection layers of one render pass: the only ones one quad can replace."""
    clusters = []
    for layer in sorted((item for item in layers if item['flat']), key=lambda item: item['depth']):
        for cluster in clusters:
            if (cluster[0]['key'] == layer['key'] and abs(cluster[0]['depth'] - layer['depth']) <= tolerance and
                    target_for(cluster[0]['depth']) == target_for(layer['depth'])):
                cluster.append(layer)
                break
        else:
            clusters.append([layer])
    groups = []
    for cluster in clusters:
        pending = list(cluster)
        while pending:
            members = [pending.pop(0)]
            frontier = list(members)
            while frontier:
                current = frontier.pop()
                joined = [other for other in pending if overlaps(current['box'], other['box'])]
                for other in joined:
                    pending.remove(other)
                members += joined
                frontier += joined
            if len(members) > 1:
                box = (min(m['box'][0] for m in members), min(m['box'][1] for m in members),
                       max(m['box'][2] for m in members), max(m['box'][3] for m in members))
                area = sum(m['area'] for m in members)
                groups.append(dict(draws=sorted(m['draw'] for m in members),
                                   owners=[m['owner'] for m in members], depth=members[0]['depth'],
                                   box=box, member_area=area, merged_area=(box[2]-box[0])*(box[3]-box[1]),
                                   members=members))
    return groups


def camera_report(sprites, entries, layers, groups, camera):
    fog = {l['draw'] for l in layers}
    totals = {}
    for index, entry in enumerate(entries):
        v = entry['values']
        if v[10] <= 0:
            continue
        pixels = projected_pixels(corners(entry, sprites[entry['sprite']]), camera, target_for(v[6]))
        if pixels > 0:
            key = f"{target_for(v[6])}/{'fog-haze' if index in fog else 'other'}"
            totals[key] = totals.get(key, 0) + pixels
    saved = 0
    for group in groups:
        before = sum(projected_pixels(m['points'], camera, target_for(m['depth'])) for m in group['members'])
        box = group['box']
        z = 38.1 + group['depth']
        merged = projected_pixels([(x, y, z) for x in (box[0], box[2]) for y in (box[1], box[3])],
                                  camera, target_for(group['depth']))
        saved += before - merged
    fog_pixels = sum(v for k, v in totals.items() if k.endswith('fog-haze'))
    all_pixels = sum(totals.values())
    return dict(camera=camera, projected_pixels={k: round(v) for k, v in sorted(totals.items())},
                saved_pixels=round(saved), saved_of_fog=saved / fog_pixels if fog_pixels else 0,
                saved_of_all=saved / all_pixels if all_pixels else 0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bank', type=Path, required=True)
    parser.add_argument('--names', type=Path, required=True, help='exported visuals directory')
    parser.add_argument('--prefix', required=True, help='export prefix, for example level6')
    parser.add_argument('--camera', action='append', required=True, help='x,y world camera, repeatable')
    parser.add_argument('--depth-tolerance', type=float, default=1e-4)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    if not output.is_relative_to((ROOT / 'build' / 'optimise-fog').resolve()):
        parser.error('output must be under build/optimise-fog')
    if not math.isfinite(args.depth_tolerance) or args.depth_tolerance < 0:
        parser.error('--depth-tolerance must be finite and not negative')
    sprites, entries, _ = load_bank(args.bank)
    name = load_names(args.names, args.prefix)
    layers = fog_layers(sprites, entries, name)
    groups = [g for g in merge_groups(layers, args.depth_tolerance) if g['merged_area'] < g['member_area']]
    cameras = [tuple(float(part) for part in text.split(',')) for text in args.camera]
    report = dict(layers=len(layers), groups=[{k: v for k, v in g.items() if k != 'members'} | dict(
                      area_ratio=g['merged_area'] / g['member_area']) for g in groups],
                  cameras=[camera_report(sprites, entries, layers, groups, camera) for camera in cameras])
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(dict(layers=report['layers'], groups=len(groups),
                          draws_removed=sum(len(g['draws']) - 1 for g in report['groups']),
                          cameras=[(c['camera'], c['saved_pixels'], round(c['saved_of_fog'], 4),
                                    round(c['saved_of_all'], 4)) for c in report['cameras']])))


if __name__ == '__main__':
    main()
