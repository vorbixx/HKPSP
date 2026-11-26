"""Extract source-backed per-placement ordinary-enemy reward overrides."""
import math
import re
import struct


MAGIC = b'HKE1'
LIMIT = 128
PERSISTENT = 1
SEMI_PERSISTENT = 2
MEGA_FLING = 4

SCENE_BASES = (
    'Crawler', 'Climber', 'Fly', 'Buzzer', 'Hatcher Baby', 'Zombie Runner',
    'Zombie Hornhead', 'Zombie Leaper', 'Worm', 'Zombie Barger', 'Roller', 'Hatcher',
    'Mushroom Brawler', 'Ruins Sentry', 'Ruins SentryB', 'Ruins Sentry Fat',
    'Ruins Sentry FatB', 'Ruins Sentry Fat B', 'Ruins Flying Sentry', 'Ruins Flying SentryB',
)
GREENPATH_BASES = (
    'Moss Walker', 'Pigeon', 'Moss Knight', 'Mosquito', 'Lazy Flyer Enemy',
    'Acid Walker', 'Mossman_Shaker', 'Moss Charger',
)
_SUFFIX = re.compile(r'^(.*?)(?: \d+| \(\d+\))?$')


def _components(group):
    """Yield (component class, export reference) from inventory component maps."""
    if isinstance(group, dict):
        for kind, refs in group.items():
            if isinstance(refs, list):
                for ref in refs:
                    yield kind, ref
            elif isinstance(refs, str):
                yield kind, refs
    elif isinstance(group, list):
        for row in group:
            if isinstance(row, dict):
                yield row.get('native_type', row.get('type')), row.get('path', row.get('file'))


def _structure(value):
    return value.get('m_Structure', value) if isinstance(value, dict) else {}


def extract(group, read, prefab=None):
    """Return reward values plus source references, or None when unsupported.

    ``read`` accepts the component export reference from an inventory row.
    """
    refs = {}
    for kind, ref in _components(group):
        if kind in ('HealthManager', 'PersistentBoolItem') and ref:
            refs[kind] = ref
    if 'HealthManager' not in refs:
        return None
    health = _structure(read(refs['HealthManager']))
    fields = ('smallGeoDrops', 'mediumGeoDrops', 'largeGeoDrops', 'effectOrigin')
    if any(k not in health for k in fields) or not isinstance(health['effectOrigin'], dict):
        return None
    counts = [health[k] for k in fields[:3]]
    if any(type(n) is not int or n < 0 for n in counts) or sum(counts) > 256:
        raise ValueError('Invalid placement Geo counts for ' + str(prefab or ''))
    origin = health['effectOrigin']
    if 'x' not in origin or 'y' not in origin:
        return None
    offset = [float(origin['x']), float(origin['y'])]
    if not all(math.isfinite(v) for v in offset):
        raise ValueError('Invalid placement effectOrigin for ' + str(prefab or ''))
    persistent = semi = False
    if 'PersistentBoolItem' in refs:
        item = _structure(read(refs['PersistentBoolItem']))
        dont_save = bool(item.get('dontSave', False))
        persistent = not dont_save
        semi = persistent and bool(item.get('semiPersistent', False))
    reward = dict(small=counts[0], medium=counts[1], large=counts[2], origin=offset,
                  persistent=persistent, semiPersistent=semi, megaFling=bool(health.get('megaFlingGeo', False)),
                  source=dict(health=refs['HealthManager']))
    if 'PersistentBoolItem' in refs:
        reward['source']['persistence'] = refs['PersistentBoolItem']
    return reward


def _is_supported(prefab):
    match = _SUFFIX.fullmatch(prefab or '')
    base = match.group(1) if match else ''
    return base in SCENE_BASES or base in GREENPATH_BASES


def supported(prefab):
    """Whether a placement belongs to a supported ordinary source archetype."""
    return _is_supported(prefab)


def write(path, spawns):
    """Write HKE1 records for supported, source-backed placement rewards."""
    records = []
    for spawn in spawns:
        reward = spawn.get('reward')
        if not reward or not _is_supported(spawn.get('prefab')):
            continue
        flags = ((PERSISTENT if reward['persistent'] else 0) | (SEMI_PERSISTENT if reward['semiPersistent'] else 0)
                 | (MEGA_FLING if reward.get('megaFling', False) else 0))
        if flags & SEMI_PERSISTENT and not flags & PERSISTENT:
            raise ValueError('Semi-persistent placement must also be persistent')
        counts = [reward[k] for k in ('small', 'medium', 'large')]
        if any(type(n) is not int or n < 0 for n in counts) or sum(counts) > 256:
            raise ValueError('Invalid placement Geo counts')
        owner = spawn['owner']
        if type(owner) is not int or owner <= 0 or owner > 0xffffffff or any(row[0] == owner for row in records):
            raise ValueError('Invalid placement owner')
        x, y = reward['origin']
        values = [float(x), float(y)]
        if not all(math.isfinite(v) for v in values):
            raise ValueError('Invalid placement effectOrigin')
        records.append((owner, *counts, *values, flags))
    if len(records) > LIMIT:
        raise ValueError('Scene has more than 128 reward overrides')
    data = bytearray(struct.pack('<4sI', MAGIC, len(records)))
    for row in records:
        data += struct.pack('<4I2fI', *row)
    path.write_bytes(data)
    return len(records)
