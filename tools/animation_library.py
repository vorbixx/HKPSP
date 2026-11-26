"""Lossless clip selection and the optional HKA1/PSA1 frame-event trailer."""
import math
import struct

EVENT = struct.Struct('<I64sif')


def complete_aliases(clips, aliases):
    """Original names take precedence; aliases may only add names, never replace them."""
    originals = {c['name']: c for c in clips if c.get('name') and c.get('frames')}
    if len(originals) != sum(bool(c.get('name') and c.get('frames')) for c in clips):
        raise ValueError('Duplicate original clip name')
    result = {name: name for name in originals}
    for alias, target in aliases.items():
        if target not in originals:
            raise ValueError(f'Alias {alias}: missing original {target}')
        if alias not in originals:
            result[alias] = target
    return result


def event_bytes(clips):
    """clips are (name, source clip, sprite ids), in bank frame-table order."""
    records, first = [], 0
    for _, clip, ids in clips:
        if len(clip['frames']) != len(ids):
            raise ValueError('Event/frame count mismatch')
        for index, frame in enumerate(clip['frames']):
            if not frame.get('triggerEvent'):
                continue
            info = frame.get('eventInfo', '').encode('utf-8')
            value = frame.get('eventFloat', 0.)
            if len(info) >= 64 or b'\0' in info or not math.isfinite(value):
                raise ValueError('Unrepresentable original frame event')
            records.append(EVENT.pack(first+index, info, frame.get('eventInt', 0), value))
        first += len(ids)
    return b'AEV1'+struct.pack('<I', len(records))+b''.join(records) if records else b''


def parse_events(data, frames):
    if not data:
        return []
    if len(data) < 8 or data[:4] != b'AEV1':
        raise ValueError('Invalid animation event header')
    count, = struct.unpack_from('<I', data, 4)
    if count > frames or len(data) != 8+count*EVENT.size:
        raise ValueError('Invalid animation event length')
    result, previous = [], -1
    for offset in range(8, len(data), EVENT.size):
        frame, info, integer, value = EVENT.unpack_from(data, offset)
        if frame <= previous or frame >= frames or b'\0' not in info or not math.isfinite(value):
            raise ValueError('Invalid animation event record')
        result.append((frame, info.split(b'\0', 1)[0].decode('utf-8'), integer, value))
        previous = frame
    return result
