"""Census drawable CLUT pages in the staged PSP route."""

import argparse
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from repack_pages import read_layout_bank
from room_stage import SCENE_ENEMY_ART, SCENE_ENEMY_RENDER, scene_enemy_banks


POOL_SLOTS = 22
GLOBALS = {
    'hero.bin': '', 'hud-visual.bin': 'hud', 'ability-panel.bin': 'ability',
    'inventory.bin': 'inventory', 'spirit.bin': 'spirit', 'crawler-anim.bin': 'crawler',
    'buzzer-anim.bin': 'buzzer', 'shade-anim.bin': 'shade', 'geo-visual.bin': 'geo',
    'great-door-visual.bin': 'great-door', 'fury-visual.bin': 'fury',
    'events-anim.bin': 'events', 'decor-fx-visual.bin': 'decor-fx',
    'lighting-atlas.bin': 'light',
}
DIRECT = ('lantern-glow.hpt', 'menu-fill.hpt', 'map-page-0.hpt')


def u32(raw, offset):
    return struct.unpack_from('<I', raw, offset)[0]


def animation_pages(raw, *, direct_sprites=False):
    sprites, clips, frames = struct.unpack_from('<3I', raw, 16)
    clip_start = 28 + 36 * sprites
    frame_start = clip_start + 68 * clips
    footer = frame_start + 4 * frames
    if raw[footer:footer+4] == b'AEV1':
        footer += 8 + 76 * u32(raw, footer+4)
    mixed = raw[footer:footer+4] == b'SCL1'
    if mixed:
        footer += 8 + 4 * sprites
    pool = None
    if footer != len(raw):
        name = raw[footer+8:]
        if (raw[footer:footer+4] != b'PPG1' or not re.fullmatch(rb'[A-Za-z0-9_-]{1,64}', name)
                or u32(raw, footer+4) != len(name)):
            raise ValueError('Invalid page pool footer')
        pool = name.decode('ascii')
    bank = read_layout_bank(raw[:footer], with_cluts=mixed)
    pages = {sprite[0] for sprite in bank['sprites']} if direct_sprites else set()
    for index in range(clips):
        first, length = struct.unpack_from('<2I', raw, clip_start+68*index+60)
        for frame in range(first, first+length):
            pages.add(bank['sprites'][u32(raw, frame_start+4*frame)][0])
    return bank['count'], pages, pool


def room_pages(raw):
    try:
        bank = read_layout_bank(raw, with_cluts=False)
    except ValueError:
        bank = read_layout_bank(raw, with_cluts=True)
    # Room sidecars can address the atlas independently of the initial draw
    # rows (e.g. plant/dynamic poses, switches and particle emitters).
    return bank['count'], {sprite[0] for sprite in bank['sprites']}, None


def uses_motes(raw):
    tag, zone = struct.unpack('<4sI', raw)
    if tag == b'HAP1' and zone in (0, 1):
        return zone == 1
    if tag == b'HAP2' and zone in (0, 5, 9, 10, 25):
        return zone == 5
    raise ValueError('Invalid scene ambient selector')


def load_trace(trace):
    files = {}
    for row in trace.read_text(encoding='utf-8-sig').splitlines():
        name, path = row.split('\t')
        if name in files:
            raise ValueError(f'Duplicate staged name: {name}')
        files[name] = Path(path)
    return files


def census(files, *, startup=False):
    pools = {}
    banks = {}

    def bank(name, stem):
        if name in banks:
            return banks[name]
        raw = files[name].read_bytes()
        try:
            if raw[:4] == b'PSA1':
                count, pages, pool = animation_pages(raw, direct_sprites=name in ('decor-fx-visual.bin', 'motes.bin'))
            elif raw[:4] == b'PSR1':
                count, pages, pool = room_pages(raw)
            elif raw[:4] == b'PSL1' and len(raw) == 200:
                count, pages, pool = u32(raw, 12), {u32(raw, 20+36*i) for i in range(5)}, None
            else:
                raise ValueError('Unrecognized visual bank')
        except (ValueError, struct.error) as error:
            raise ValueError(f'{name}: {error}') from error
        if pool and pools.setdefault(pool, count) != count:
            raise ValueError(f'{name}: inconsistent {pool} size')
        prefix = pool or stem
        paths = [f'{prefix+"-" if prefix else ""}page-{i}.hpt' for i in range(count)]
        for path in paths:
            if path not in files:
                raise ValueError(f'{name}: missing page {path}')
        banks[name] = frozenset(paths[index] for index in pages)
        return banks[name]

    common = {name: bank(name, stem) for name, stem in GLOBALS.items()}
    for name in DIRECT:
        if name not in files:
            raise ValueError(f'Missing in-room page: {name}')
        common[name] = frozenset((name,))
    fonts = sum(bool(re.fullmatch(r'font-page-\d+\.hpt', name)) for name in files)
    if fonts != 2:
        raise ValueError(f'Expected 2 fixed font pages, found {fonts}')

    for name in (('static-room-visual.bin',) if startup else sorted(files)):
        match = re.fullmatch(r'rooms/([^/]+)/static-room-visual\.bin', name)
        if not match and not startup:
            continue
        scene = 'Tutorial_01' if startup else match[1]
        prefix = '' if startup else f'rooms/{scene}/'
        scene_prefix = f'scenes/{scene}/'
        demand = dict(common)
        assets = (name,) if startup else (asset for asset in files if asset.startswith(prefix) and asset.endswith('-visual.bin'))
        for asset in assets:
            demand[asset] = bank(asset, asset.removesuffix('-visual.bin'))
        if scene in ('Town', 'Crossroads_01', 'Fungus1_01'):
            demand['region-title.bin'] = bank('region-title.bin', 'region-title')
        ambient = files.get(scene_prefix+'scene-ambient.bin')
        if scene == 'Tutorial_01' or (ambient and uses_motes(ambient.read_bytes())):
            demand['motes.bin'] = bank('motes.bin', 'mote')
        greenpath = files.get(scene_prefix+'greenpath-enemies.bin')
        table = scene_prefix+'scene.bin'
        enemy_names = set() if startup or (scene == 'Tutorial_01' and table not in files) else scene_enemy_banks(files[table])
        if greenpath:
            header = greenpath.read_bytes()[:8]
            if len(header) != 8 or header[:4] not in (b'HKG1', b'HKG2'):
                raise ValueError(f'Invalid Greenpath metadata: {greenpath}')
            if u32(header, 4):
                enemy_names.add('greenpath-art.bin')
        for stem, _ in SCENE_ENEMY_RENDER:
            leaf = SCENE_ENEMY_ART.get(stem, f'{stem}-anim.bin')
            if leaf in enemy_names:
                asset = 'rooms/scene-enemy/'+leaf
                demand[asset] = bank(asset, 'rooms/scene-enemy/'+stem)
        yield scene, frozenset().union(*demand.values()), fonts, demand


def manifest_rows(pages, fixed, demand):
    # Room-local actor banks are loaded only when their saved-state actor is
    # present (Town's Tiso/Elderbug, rescued NPCs, tolls, and similar art).
    # Keep their pages in the room preload set when loaded, but do not reject a
    # valid transition when the runtime texture registry omits that actor.
    def required_bank(name):
        return (not name.startswith('rooms/') or
                name.endswith('/static-room-visual.bin'))

    required = frozenset().union(fixed, *(paths for name, paths in demand.items()
                                          if required_bank(name)))
    return ''.join(('' if name in required else '?') + name + '\n' for name in sorted(pages | fixed))


def write_manifests(destination):
    files = {path.relative_to(destination).as_posix(): path
             for path in destination.rglob('*') if path.is_file()}
    if 'static-room-visual.bin' not in files:
        return
    fixed = {name for name in files if re.fullmatch(r'font-page-\d+\.hpt', name)}
    for startup in (True, False):
        for scene, pages, _, demand in census(files, startup=startup):
            path = destination / ('room-pages-startup.txt' if startup else f'rooms/{scene}/room-pages.txt')
            path.write_text(manifest_rows(pages, fixed, demand), encoding='ascii', newline='\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path)
    parser.add_argument('output', type=Path, nargs='?')
    parser.add_argument('--manifests', action='store_true')
    args = parser.parse_args()
    if args.manifests:
        write_manifests(args.trace)
        return
    if args.output is None:
        parser.error('output is required for a census')
    rows = list(census(load_trace(args.trace)))
    with args.output.open('w', encoding='utf-8', newline='\n') as output:
        output.write('room\tpooled_pages\tpool_slots\tover_by\tfixed_fonts\ttop_banks\tpages\n')
        for scene, pages, fonts, demand in rows:
            top = sorted(demand, key=lambda name: (-len(demand[name]), name))[:4]
            names = ', '.join(f'{Path(name).stem}:{len(demand[name])}' for name in top)
            output.write(f'{scene}\t{len(pages)}\t{POOL_SLOTS}\t{max(0,len(pages)-POOL_SLOTS)}'
                         f'\t{fonts}\t{names}\t{",".join(sorted(pages))}\n')
    print(f'{len(rows)} rooms, {sum(len(row[1]) > POOL_SLOTS for row in rows)} over budget, '
          f'peak {max(len(row[1]) for row in rows)} pooled pages')


if __name__ == '__main__':
    main()
