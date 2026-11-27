"""Validate run-3 telemetry and derive post-startup reports (no timing gates)."""
import argparse
from collections import Counter
import csv
import hashlib
import json
import math
import struct
from pathlib import Path

CONFIGS = ['baseline', 'c1', 'c2', 'c3', 'shipped', 'strips', 'page_fix', 'quad_cache', 'all', 'io_22', 'io_30']
# Default perf build (no HKPSP_PERF_SWEEP): the shipped configuration only.
SHIPPED_ONLY = ['shipped']
ROOM_GE_AB = ['room_exact', 'room_ge']
REQUESTED = dict(zip(CONFIGS, [(0, 0, 0)] * 5 + [(1, 0, 0), (0, 1, 0), (0, 0, 1), (1, 1, 1)] + [(0, 0, 0)] * 2))
REQUESTED.update(dict.fromkeys(ROOM_GE_AB, (0, 0, 0)))
COLUMNS =['vblank_delta', 'page_loads', 'page_open_us', 'page_read_us', 'page_verify_us',
           'page_load_us', 'page_evictions', 'resident_bytes', 'blocking_miss',
           'texture_read_bytes', 'ge_scene_us', 'ge_grade_us', 'ge_hud_us',
           'arena_bytes', 'largest_free_block', 'audio_read_bytes',
           'audio_stream_underruns', 'audio_tail_underruns', 'event_nail_hits',
           'event_decor_cuts', 'event_damage_taken', 'event_pickup_geo']
EVENTS = COLUMNS[-4:]
RUNTIME_COLUMNS = ['game_run_us', 'mixer_run_us', 'audio_io_run_us', 'idle_run_us', 'kernel_interrupt_us', 'runtime_excess_us', 'runtime_probe_errors', 'simulation_run_us', 'draw_list_run_us', 'gu_run_us', 'audio_submit_us', 'perf_bookkeeping_us', 'swap_us', 'cpu_scene_us', 'cpu_grade_us', 'cpu_hud_us', 'runtime_probe_us', 'runtime_probe_calls']
COLUMNS += RUNTIME_COLUMNS + ['total_us', 'input_us', 'audio_read_calls', 'audio_read_us', 'audio_read_cpu_us', 'audio_pump_wakeups', 'audio_cpu_probe_errors']


def require(ok, message):
    if not ok:
        raise ValueError(message)


def read(path):
    with path.open(newline='') as file:
        return list(csv.DictReader(file))


def write(path, fields, rows):
    with path.open('w', newline='') as file:
        writer = csv.writer(file)
        writer.writerow(fields)
        writer.writerows(rows)


def summarize(rows, configs=CONFIGS):
    """Nearest-rank percentiles, dropping frame 0 separately in each config."""
    result, vblanks = [], []
    for config in configs:
        group = [r for r in rows if r['config'] == config and int(r['frame']) != 0]
        require(len(group) == 599, f'Incomplete post-startup config {config}')
        for row in group:
            row['work_us'] = int(row['total_us']) - int(row['vblank_us'])
        for column in group[0]:
            if column in ('config', 'frame', 'page_loaded_ids'):
                continue
            values = sorted(int(r[column]) for r in group)
            result.append([config, column, len(values), *[
                values[math.ceil(len(values) * p) - 1] for p in (.5, .95, .99)], values[-1]])
        vblanks.extend([config, count, frames] for count, frames in
                       sorted(Counter(int(r['vblank_delta']) for r in group).items()))
    return result, vblanks


def validate(path, combat=False):
    rows = read(path / 'cpu.csv')
    manifest = read(path / 'run3-configs.csv')
    names = [r['config'] for r in manifest]
    require(names in (CONFIGS, SHIPPED_ONLY, ROOM_GE_AB), 'Invalid run-3 config manifest')
    configs = names
    for row in manifest:
        require(tuple(row[k] for k in ('strips_requested', 'page_fix_requested', 'quad_cache_requested'))
                == tuple(map(str, REQUESTED[row['config']])) and row['implementation'] == 'stub', 'Unrecognized run-3 switches')
        require(row.get('audio_io_priority') == str(0x30 if row['config'] == 'io_30' else 0x22), 'Invalid audio I/O priority')
        require(row.get('no_streaming_reads') == '0' and int(row.get('preload_pcm_bytes', '0')) > 16777216, 'Invalid no-read omission')
    require(len(rows) == 600 * len(configs), f'Run-3 requires {600 * len(configs)} CPU rows')
    for c, config in enumerate(configs):
        group = rows[c * 600:(c + 1) * 600]
        for frame, row in enumerate(group):
            require(row['config'] == config and row['frame'] == str(frame), 'Run-3 frame order')
            for column in COLUMNS + ['sync_us', 'vblank_us']:
                value = row.get(column, '')
                require(value.isascii() and value.isdecimal() and 0 <= int(value) <= 0xffffffff,
                        f'Invalid run-3 counter {config}/{frame}/{column}')
            require(int(row['total_us']) >= int(row['vblank_us']), 'Invalid work interval')
            require(int(row['runtime_probe_errors']) == 0, 'Runtime probe failed')
            require(int(row['runtime_probe_calls']) > 0, 'Missing runtime probes')
            require(int(row['total_us']) == sum(int(row[k]) for k in ('game_run_us', 'mixer_run_us', 'audio_io_run_us', 'idle_run_us', 'kernel_interrupt_us')) - int(row['runtime_excess_us']), 'Runtime accounting mismatch')
            require(int(row['vblank_delta']) > 0, 'Missing vblank delta')
            require(int(row['page_open_us']) + int(row['page_read_us']) + int(row['page_verify_us'])
                    == int(row['page_load_us']), 'Page load phase accounting')
            require(int(row['blocking_miss']) == int(int(row['page_loads']) > 0), 'Blocking page accounting')
        if combat:
            for event in EVENTS:
                require(sum(int(r[event]) for r in group) > 0, f'Combat tape missing {event}: {config}')
    events = Counter()
    for event in read(path / 'page-events.csv'):
        require(event['config'] in configs and event['event'] in ('load', 'evict'), 'Invalid page event')
        require(event['page'].isdecimal() and 0 <= int(event['frame']) < 600, 'Invalid page id/frame')
        events[event['config'], event['frame'], event['event']] += 1
    for row in rows:
        for event, column in [('load', 'page_loads'), ('evict', 'page_evictions')]:
            require(events[row['config'], row['frame'], event] == int(row[column]), 'Page events disagree with counters')
    raw = read(path / 'ms-raw.csv')
    require(len(raw) == 2 and [r['requests'] for r in raw] == ['1', '4'], 'Raw MS request counts')
    for row in raw:
        require(row['bytes'] == '262144' and all(row[k].isdecimal() for k in
                ('open_us', 'read_us', 'checksum')), 'Invalid raw MS figures')
    require(raw[0]['checksum'] == raw[1]['checksum'], 'Raw MS read contents differ')
    reference = 'room_exact' if configs == ROOM_GE_AB else 'shipped'
    shipped = (path / f'cpu-{reference}.ppm').read_bytes()
    require(shipped.startswith(b'P6\n480 272\n255\n') and len(shipped.split(b'\n', 3)[3]) == 480*272*3,
            f'Invalid {reference} PPM')
    parity = []
    for config in [c for c in configs if c not in ('baseline', 'c1', 'c2', 'c3', 'shipped', reference)]:
        other = (path / f'cpu-{config}.ppm').read_bytes()
        require(other.startswith(b'P6\n480 272\n255\n') and len(other) == len(shipped),
                f'Invalid {config} PPM')
        different = sum(a != b for a, b in zip(other, shipped))
        # World-space GE rounding is reviewed visually; existing sweep modes
        # retain their byte-exact preview contract.
        if configs != ROOM_GE_AB:
            require(different == 0, f'Run-3 PPM mismatch: {config}')
        parity.append([config, different, hashlib.sha256(other).hexdigest()])
    summary, vblanks = summarize(rows, configs)
    write(path / 'run3-summary.csv', ['config', 'column', 'frames', 'median', 'p95', 'p99', 'max'], summary)
    write(path / 'run3-vblanks.csv', ['config', 'vblank_delta', 'frames'], vblanks)
    write(path / 'run3-parity.csv', ['config', 'different_bytes', 'sha256'], parity)
    write(path / 'run3-audio.csv', ['config', 'frame', 'kind', 'underruns'],
          ([r['config'], r['frame'], kind, r[column]] for r in rows for kind, column in
           [('stream', 'audio_stream_underruns'), ('tail', 'audio_tail_underruns')] if int(r[column])))
    peaks = {}
    for config in configs:
        group = [r for r in rows if r['config'] == config]
        peak = max(group, key=lambda r: int(r['arena_bytes']))
        peaks[config] = {key: int(peak[key]) for key in ('frame', 'arena_bytes', 'largest_free_block')}
    (path / 'run3-memory.json').write_text(json.dumps(peaks, indent=2) + '\n')
    return len(rows)


def emit_tape(output):
    source = Path(__file__).resolve().parents[2] / 'tests/psp/data/run3-combat.json'
    data = json.loads(source.read_text())
    segments = data['segments']
    require(sum(s[0] for s in segments) == 1200, 'Combat tape must contain 1200 control ticks')
    require(all(s[0] > 0 and not s[1] & ~3579 and s[2:] == [0, 0] for s in segments), 'Invalid ordinary controls')
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(b'HKIR0001' + struct.pack('<I', len(segments)) +
                       b''.join(struct.pack('<IIhh', *s) for s in segments))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('path', type=Path)
    parser.add_argument('--combat', action='store_true')
    parser.add_argument('--emit-combat-tape', action='store_true')
    args = parser.parse_args()
    if args.emit_combat_tape:
        emit_tape(args.path)
        print(f'Combat tape written: {args.path}')
    else:
        print(f'Run-3 validated: {validate(args.path, args.combat)} frames; summaries exclude frame 0; switches are stubs')
