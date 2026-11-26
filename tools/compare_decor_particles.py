"""Validate owner-scoped Unity snapshots and compare emitter distributions to host CSV.

The random streams differ. These are measured distributions and tracked Unity
trajectories, never a particle-index match or a visual acceptance tolerance.
"""
import argparse
import csv
import hashlib
import json
import math
from collections import defaultdict
from pathlib import Path
from statistics import mean


def angle(x, y):
    return math.degrees(math.atan2(y, x))


def angle_difference(a, b):
    return (a-b+180) % 360-180


def distribution(values):
    values = sorted(values)
    if not values:
        return None
    return dict(min=values[0], mean=mean(values), max=values[-1],
                p10=values[int((len(values)-1)*.1)], p90=values[int((len(values)-1)*.9)])


def validate_snapshots(path, frames, owner, target, part_names, frame_rows):
    rows = [json.loads(line) for line in Path(path).read_text().splitlines()]
    timeline = {int(r['offset']): int(r['unityFrame']) for r in frame_rows}
    expected = {(f, p) for f in frames for p in range(len(part_names))}
    actual = [(r['offset'], r['part']) for r in rows]
    # These fixtures have one system on each debris root. Reject a changed
    # hierarchy rather than silently assigning a nested system to the root.
    if len(actual) != len(set(actual)) or set(actual) != expected:
        raise ValueError('Missing/duplicate particle system offsets or changed debris hierarchy')
    identities = {}
    def finite(value):
        if isinstance(value, float) and not math.isfinite(value):
            raise ValueError('Nonfinite particle state')
        if isinstance(value, dict):
            for v in value.values():
                finite(v)
        elif isinstance(value, list):
            for v in value:
                finite(v)
    for r in rows:
        finite(r)
        if r['owner'] != owner or r['target'] != target or r['path'].split('/')[-1] != part_names[r['part']]:
            raise ValueError('Particle owner/path mismatch')
        if r['unityFrame'] != timeline[r['offset']]:
            raise ValueError('Particle frame drift')
        if identities.setdefault(r['part'], r['instance']) != r['instance']:
            raise ValueError('Particle system identity changed')
        if r['count'] != len(r['particles']):
            raise ValueError('Particle count mismatch')
        if len({p['seed'] for p in r['particles']}) != r['count']:
            raise ValueError('Ambiguous particle seed identity')
        if r['simulationSpace'] not in ('World', 'Local', 'Custom'):
            raise ValueError('Unknown particle simulation space')
        for p in r['particles']:
            for key in ('position', 'worldPosition', 'velocity', 'totalVelocity', 'worldVelocity', 'startSize', 'size'):
                if len(p[key]) != 3:
                    raise ValueError('Invalid particle vector')
            if len(p['color']) != 4 or len(p['startColor']) != 4 or not 0 <= p['remainingLifetime'] <= p['startLifetime']:
                raise ValueError('Invalid particle color/lifetime')
    return rows


def authored_hit_matrix(source, source_manifest, owner, part):
    """Apply Breakable.Start + Break + Extensions.SetRotationZ to the chain.

    Kept independent of the production converter as a transform oracle.
    """
    from convert_room_visuals import project
    source = Path(source)
    room = source.parent / 'room-visuals'
    room_manifest = json.loads((room / 'room-visuals-map.json').read_text())
    def read(name):
        path = source / name
        expected = source_manifest['source_sha256'].get(name)
        if not path.exists():
            path = room / name
            expected = room_manifest['source_sha256'][name]
        if sha(path) != expected:
            raise ValueError('Changed authored transform: ' + name)
        return json.loads(path.read_text())
    def transform(name):
        for c in read(name)['m_Components']:
            key = f"level6-{c['m_Component']['m_PathID']}.json"
            if 'm_LocalRotation' in read(key):
                return key
        raise ValueError('Missing transform')
    record = next(r for r in source_manifest['derived']['records'] if r['owner'] == owner)
    root = read(transform(f'level6-{owner}.json'))
    signed_offset = read(record['breakable'])['m_Structure']['angleOffset'] * math.copysign(1, root['m_LocalScale']['m_X'])
    chain = []
    key = transform(record['debris'][part])
    while key:
        t = read(key)
        chain.append(t)
        father = t['m_Father']
        if father['m_FileID']:
            raise ValueError('Nonlocal transform ancestor')
        key = f"level6-{father['m_PathID']}.json" if father['m_PathID'] else None
    q = chain[0]['m_LocalRotation']
    if abs(q['m_X']) > 1e-6 or abs(q['m_Y']) > 1e-6:
        raise ValueError('Nonplanar fixture')
    local_angle = 2*math.atan2(q['m_Z'], q['m_W']) + math.radians(signed_offset)
    chain[0]['m_LocalRotation'] = dict(m_X=0, m_Y=0, m_Z=math.sin(local_angle/2), m_W=math.cos(local_angle/2))
    origin = project((0, 0, 0), chain)
    columns = [tuple(a-b for a, b in zip(project(v, chain), origin))
               for v in ((1, 0, 0), (0, 1, 0), (0, 0, 1))]
    matrix = [columns[c][r] if c < 3 else origin[r] for r in range(3) for c in range(4)] + [0, 0, 0, 1]
    return matrix, signed_offset


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def compare(captures, host_path, manifest_path, source, output):
    host = defaultdict(list)
    with Path(host_path).open() as stream:
        for r in csv.DictReader(stream):
            if r['kind'] == 'debris':
                host[int(r['owner']), int(r['emitter']), int(r['frame'])].append(r)
    manifest = json.loads(Path(manifest_path).read_text())
    source_manifest_path = source / 'decor-effects.json'
    source_manifest = json.loads(source_manifest_path.read_text())
    report = dict(method='owner/part to authored emitter; different RNG streams; no index pairing',
                  inputs={str(p): sha(p) for p in (host_path, manifest_path, source_manifest_path,
                      source.parent / 'room-visuals/room-visuals-map.json', Path(__file__))},
                  emitters=[], frames=[], tracks=[], limitations=[
                      'Distribution differences alone are not proof of a bug or visual acceptance.',
                      'Unity particle seed pairs are used only within one system/capture.',
                      'Births span the emission interval; host debris is an immediate burst.',
                      'Shape, transform and simulation are measured; material lighting is not.'])
    for capture in captures:
        provenance = json.loads((capture / 'provenance.json').read_text())
        if not provenance['complete'] or not provenance['original_inputs_unchanged'] or str(provenance['build']) != '22529139':
            raise ValueError('Incomplete/unverified capture')
        for name in ('particles.jsonl', 'probe-frames.csv', 'probe-provenance.json'):
            if sha(capture / name) != provenance['files'][name]:
                raise ValueError('Changed capture: ' + name)
        config = provenance['configuration']
        owner, target = config['owner'], config['target']
        record = next(r for r in source_manifest['derived']['records'] if r['owner'] == owner)
        names = [json.loads((source / n).read_text())['m_Name'] for n in record['debris']]
        with (capture / 'probe-frames.csv').open() as stream:
            frame_rows = list(csv.DictReader(stream))
        rows = validate_snapshots(capture / 'particles.jsonl', config['frames'], owner, target, names, frame_rows)
        for name in ('particles.jsonl', 'provenance.json'):
            report['inputs'][str(capture / name)] = sha(capture / name)
        profile = next(p for p in manifest['profiles'] if p['owner'] == owner)
        previous = {}
        for r in rows:
            part, f = r['part'], r['offset']
            emitter = profile['emitters'][part]
            e = manifest['emitters'][emitter]
            if e['owner'] != record['debris'][part]:
                raise ValueError('Host emitter mapping mismatch')
            v = e['values']
            native = r['particles']
            port = host[owner, emitter, f]
            if not port:
                raise ValueError('Missing host emitter/frame')
            summary = dict(owner=owner, part=part, emitter=emitter, offset=f,
                           unity_count=len(native), host_count=len(port),
                           unity_age=distribution([p['startLifetime']-p['remainingLifetime'] for p in native]),
                           host_floor_count=sum(abs(float(p['y'])-profile['position'][1]) < 1e-5 for p in port),
                           unity_below_host_floor=sum(p['worldPosition'][1] < profile['position'][1] for p in native))
            for axis, a in enumerate(('x', 'y')):
                summary['unity_'+a] = distribution([p['worldPosition'][axis] for p in native])
                summary['host_'+a] = distribution([float(p[a]) for p in port])
                summary['unity_v'+a] = distribution([p['worldVelocity'][axis] for p in native])
                summary['host_v'+a] = distribution([float(p['v'+a])+float(p['module'+a.upper()]) for p in port])
            report['frames'].append(summary)
            if f == config['frames'][0]:
                matrix, signed_offset = authored_hit_matrix(source, source_manifest, owner, part)
                actual = r['localToWorld']
                matrix_error = max(abs(a-b) for a, b in zip(matrix, actual))
                if matrix_error > 1e-4:
                    raise ValueError('Authored post-hit transform model disagrees with Unity')
                native_angles, host_angles, position_errors = [], [], []
                for i in range(101):
                    u = i/100
                    theta = math.radians(r['shape']['arc'])*u
                    native_angles.append(angle(actual[0]*math.cos(theta)+actual[1]*math.sin(theta),
                                               actual[4]*math.cos(theta)+actual[5]*math.sin(theta)))
                    native_offset = [r['shape']['radius']*(actual[k]*math.cos(theta)+actual[k+1]*math.sin(theta))
                                     for k in (0, 4)]
                    if 'bases' in e:  # HFX5 right-hit radius-scaled columns.
                        b = e['bases'][8:12]
                        host_offset = [b[a]*math.cos(theta)+b[a+2]*math.sin(theta) for a in (0,1)]
                        host_angles.append(angle(*host_offset))
                    else:  # Historical HFX4 snapshots retain the old diagnosis.
                        host_angles.append(math.degrees(v[3]+v[4]*(u-.5)) +
                                           profile['angle']*(-1 if v[4] < 0 else 1))
                        h = math.radians(host_angles[-1])
                        host_offset = [v[5]*math.cos(h), v[5]*math.sin(h)]
                    position_errors.append(math.hypot(*(a-b for a, b in zip(host_offset, native_offset))))
                deltas = [angle_difference(h, n) for h, n in zip(host_angles, native_angles)]
                report['emitters'].append(dict(owner=owner, part=part, emitter=emitter,
                    particle_source=e['source'], debris_source=e['owner'], simulationSpace=r['simulationSpace'],
                    gravity=r['gravity'], gravityModifier=r['gravityModifier'], force=r['force'], limit=r['limit'],
                    collision=r['collision'], velocityModule=r['velocityModule'],
                    authored_signed_local_offset=signed_offset, post_hit_matrix_error=matrix_error,
                    unity_arc_endpoints=[native_angles[0], native_angles[-1]],
                    host_arc_endpoints=[host_angles[0], host_angles[-1]],
                    arc_angle_error=distribution(deltas),
                    shape_boundary_position_error=distribution(position_errors),
                    measured_base_angles=distribution([angle(*p['velocity'][:2]) for p in native]),
                    host_base_angles=distribution([angle(float(p['vx']), float(p['vy'])) for p in port]),
                    unity_mean_base_vx=mean(p['velocity'][0] for p in native),
                    host_mean_base_vx=mean(float(p['vx']) for p in port)))
            current = {p['seed']: p for p in native}
            if part in previous:
                prior, prior_frame = previous[part]
                for seed in sorted(current.keys() & prior.keys()):
                    a, b = prior[seed], current[seed]
                    dt = (f-prior_frame)/60
                    report['tracks'].append(dict(owner=owner, emitter=emitter, seed=seed,
                        start=prior_frame, end=f, dt=dt,
                        dx=b['worldPosition'][0]-a['worldPosition'][0],
                        dy=b['worldPosition'][1]-a['worldPosition'][1],
                        ax=(b['worldVelocity'][0]-a['worldVelocity'][0])/dt,
                        ay=(b['worldVelocity'][1]-a['worldVelocity'][1])/dt))
            previous[part] = current, f
    output.mkdir(parents=True, exist_ok=True)
    (output / 'comparison.json').write_text(json.dumps(report, indent=2)+'\n')
    keys = ['owner', 'emitter', 'offset', 'unity_count', 'host_count',
            'unity_x_min', 'unity_x_mean', 'unity_x_max', 'host_x_min', 'host_x_mean', 'host_x_max',
            'unity_y_min', 'unity_y_mean', 'unity_y_max', 'host_y_min', 'host_y_mean', 'host_y_max',
            'host_floor_count', 'unity_below_host_floor']
    with (output / 'comparison.csv').open('w', newline='') as stream:
        writer = csv.DictWriter(stream, keys)
        writer.writeheader()
        for r in report['frames']:
            flat = {k: r[k] for k in keys if k in r}
            for prefix in ('unity_x', 'host_x', 'unity_y', 'host_y'):
                for stat in ('min', 'mean', 'max'):
                    flat[prefix+'_'+stat] = r[prefix][stat] if r[prefix] else None
            writer.writerow(flat)
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--captures', nargs='+', type=Path, required=True)
    parser.add_argument('--host', type=Path, default=Path('_private/research/decor-particle-measurement/host-before.csv'))
    parser.add_argument('--manifest', type=Path, default=Path('_private/research/decor-particle-measurement/host-manifest-before.json'))
    parser.add_argument('--source', type=Path, default=Path('_private/research/decor-effects'))
    parser.add_argument('--output', type=Path, default=Path('build/decor-particle-measurement'))
    args = parser.parse_args()
    result = compare(args.captures, args.host, args.manifest, args.source, args.output)
    print(json.dumps(dict(emitters=len(result['emitters']), frames=len(result['frames']), tracks=len(result['tracks']))))
