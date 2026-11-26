"""Pack original Greenpath animation snapshots and append-only scene spawns.

Explicit --extract reads the private game copy using UnityPy. Normal regeneration
only repacks the saved snapshot, without running the original or an extractor.
"""
import converted_files  # Install hardlink copy-on-write before asset writes.
import argparse
import json
import math
import shutil
import struct
from pathlib import Path
from assetripper_client import ROOT
from convert_crossroads_bosses import convert as pack
from tool_paths import TEX3DS as TEX3DS_EXE

# IDs refer to the copied Steam build 22529139. Order is GreenpathKind's ABI.
ACTORS = [
    ('Moss Walker', 'Mosscreep', 128, 'sharedassets128.assets', 222,
     dict(Idle='Rest', Move='Walk', Turn='Turn', Antic='Appear', Death='Death Air')),
    ('Pigeon', 'Maskfly', 128, 'sharedassets128.assets', 206,
     dict(Idle='Idle 01', Move='Fly')),
    ('Moss Knight', 'MossKnight', 149, 'sharedassets149.assets', 104,
     dict(Idle='Idle', Move='Walk', Antic='Unshield Front', Attack='Slash Quick', Death='Death Air',
          Turn='Turn', ShieldFront='Shield Front', ShieldTop='Shield Top', BumpFront='Shield Front Bump',
          BumpTop='Shield Top Bump', UnshieldTop='Unshield Top', Slash='Slash', Slash2='Slash 2',
          SlashEnd='Slash End', Evade='Evade', Shoot='Shoot', ShootEnd='Shoot End')),
    ('Mosquito', 'Squit', 130, 'sharedassets27.assets', 795,
     dict(Idle='Idle', Antic='Attack Antic', Attack='Attack', Death='Death Air')),
    ('Lazy Flyer Enemy', 'Aluba', 154, 'sharedassets154.assets', 12,
     dict(Idle='Fly', Move='Glide')),
    ('Acid Walker', 'Durandoo', 140, 'sharedassets140.assets', 158,
     dict(Idle='Walk', Move='Walk', Turn='Turn', Death='Death Air')),
    ('Mossman_Shaker', 'VolatileMosskin', 128, 'sharedassets128.assets', 220,
     dict(Idle='Idle', Move='Walk', Turn='Turn', Attack='Attack', AttackEnd='Attack End', Death='Death Air')),
    ('Moss Charger', 'MossCharger', 139, 'sharedassets139.assets', 37,
     dict(Idle='Disappear', Antic='Appear', Attack='Charge', Recover='Disappear', Stun='Stun', Death='Death Air')),
    ('Hornet Boss 1', 'Hornet', 133, 'sharedassets105.assets', 69,
     dict(Idle='Idle', Antic='G Dash Antic', Attack='G Dash', Jump='Jump', AirAttack='A Dash',
          Throw='Throw', Sphere='Sphere Attack', Stun='Stun', Death='Death Air',
          JumpAntic='Jump Antic', AirAntic='A Dash Antic', ThrowAntic='Throw Antic',
          SphereAntic='Sphere Antic G', SphereBall='Sphere Ball', SphereFlash='Flash', Recover='G Dash Recover1',
          EvadeAntic='Evade Antic', Evade='Evade', Escape='Jump', Flourish='Flourish')),
]
ALIASES = {prefix: (file, library, clips) for _, prefix, _, file, library, clips in ACTORS}
ALIASES['Needle'] = ('sharedassets105.assets', 69, dict(Idle='Needle'))


def require_sphere_flash(bank):
    if b'Hornet/SphereFlash\0' not in bank:
        raise ValueError('Greenpath snapshot lacks SphereFlash; refresh it with --extract')


def kind_for_prefab(name):
    for kind, (prefab, *_) in enumerate(ACTORS):
        if name == prefab or name.startswith(prefab + ' ') or name.startswith(prefab + '('):
            return kind
    # Non-damaging scene Aluba and its damage-capable prefab share the same art.
    if name == 'Lazy Flyer' or name.startswith('Lazy Flyer ('):
        return 4
    return None


def write_scene_spawns(target, spawns, bounds, hornet_room=None):
    selected = []
    for spawn in spawns:
        kind = kind_for_prefab(spawn['prefab'])
        if kind is None or (not spawn['active'] and kind != 8):
            continue
        x, y = spawn['position']
        facing = spawn['scale'][0]
        if not all(math.isfinite(v) for v in (x, y, facing)):
            raise ValueError('Non-finite Greenpath spawn')
        selected.append((kind, x, y, facing))
    if len(selected) > 24:
        raise ValueError('Greenpath room exceeds the bounded actor pool')
    arena = hornet_room['arena'] if hornet_room else bounds
    data = bytearray(struct.pack('<4sI4f', b'HKG2' if hornet_room else b'HKG1', len(selected), *arena))
    if hornet_room:
        data += struct.pack('<6f', *hornet_room['trigger'], *hornet_room['intro'])
    for row in selected:
        data += struct.pack('<I3f', *row)
    (target / 'greenpath-enemies.bin').write_bytes(data)


def profiles(output, snapshot, extract):
    source = output / 'source-components.json' if extract else snapshot / 'source-components.json'
    rows = json.loads(source.read_text())
    speeds = (2, 2, 3, 3, 2, 2, 2, 9, 8)
    gravity = (30, 0, 30, 0, 0, 0, 30, 30, 30)
    result = []
    for i, (prefab, prefix, *_) in enumerate(ACTORS):
        components = [r['data'] for r in rows if r['actor'] == prefab and 'data' in r]
        health = next(c for c in components if 'hp' in c and 'smallGeoDrops' in c)
        boxes = [r for r in rows if r['actor'] == prefab and r['native_type'] == 'BoxCollider2D' and 'data' in r]
        boxes.sort(key=lambda r: 'local_position' in r)
        box = boxes[0]['data'] if boxes else None
        if box is None:
            raise ValueError('Missing original body collider for ' + prefab)
        offset, size = dict(box['m_Offset']), box['m_Size']
        position = next((r.get('local_position', [0,0]) for r in rows if r.get('data') is box), [0,0])
        offset['x'] += position[0]; offset['y'] += position[1]
        body = [offset['x']-size['x']/2, offset['y']-size['y']/2,
                offset['x']+size['x']/2, offset['y']+size['y']/2]
        result.append(dict(prefix=prefix, hp=health['hp'], damage=0 if i in (1,4) else 1,
                           geo=health['smallGeoDrops']+5*health['mediumGeoDrops']+25*health['largeGeoDrops'],
                           body=body, speed=speeds[i], gravity=gravity[i], invincible=bool(health.get('invincible', False)) if i == 5 else False,
                           direction=health.get('invincibleFromDirection', 0)))
    def number(v):
        return format(float(v), '.8f') + 'f'
    lines = ['// Generated by tools/convert_greenpath_enemies.py; original HP/Geo/body, native movement tuning.',
             'const GreenpathProfile profiles[]={']
    for p in result:
        lines.append('    {"%s",%d,%d,%d,{%s},%s,%s,%s,%d},' %
                     (p['prefix'], p['hp'], p['damage'], p['geo'], ','.join(map(number, p['body'])),
                      number(p['speed']), number(p['gravity']), str(p['invincible']).lower(), p['direction']))
    lines.append('};')
    (ROOT / 'include/greenpath_enemy_data.hpp').write_text('\n'.join(lines)+'\n', newline='\n')
    if extract:
        shutil.copy2(source, snapshot / 'source-components.json')
    return result


def convert(args):
    snapshot = args.snapshot
    if args.extract:
        args.snapshot = None
        args.save_snapshot = snapshot
    else:
        args.save_snapshot = None
    scenes = [(f'level{scene}', {prefab}) for prefab, _, scene, *_ in ACTORS]
    # Moss Knight Shoot spawns the Grass Ball prefab (sharedassets149 GameObject 34, Sprite 20).
    pack(args, ALIASES, 'greenpath', scenes, complete_library=False,
         unity_sprites=(('MossKnight/Spit', 'sharedassets149.assets', 20),))
    require_sphere_flash((args.output / 'romfs/greenpath-art.bin').read_bytes())
    result = profiles(args.output, snapshot, args.extract)
    manifest_path = args.output / 'manifest.json'
    manifest = json.loads(manifest_path.read_text())
    manifest['profiles'] = result
    manifest['limitations'] = ['Native approximate FSM sequencing and movement tuning.',
        'Quarter-resolution original sprite frames; original world geometry and clip timing.',
        'Aluba death reuses flight; separate death particles/audio are not included.']
    if manifest['texture_bytes'] > 6*1024*1024:
        raise ValueError('Greenpath art exceeds the 6 MiB encounter budget')
    manifest_path.write_text(json.dumps(manifest, indent=2)+'\n')
    if args.extract:
        shutil.copy2(manifest_path, snapshot / 'manifest.json')
    if args.romfs:
        args.romfs.mkdir(parents=True, exist_ok=True)
        for path in (args.output / 'romfs').glob('greenpath-*'):
            shutil.copy2(path, args.romfs / path.name)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--extract', action='store_true')
    p.add_argument('--source', type=Path, default=ROOT/'_private/original/Hollow Knight/hollow_knight_Data')
    # The snapshot lives in this checkout's own private tree: the shared HK3DS research copy is
    # read-only here and predates the Moss Knight shield, evade, second slash and spit clips.
    p.add_argument('--snapshot', type=Path, default=ROOT/'_private/converted/greenpath-enemies-source')
    p.add_argument('--output', type=Path, default=ROOT/'_private/converted/greenpath-enemies')
    p.add_argument('--packages', type=Path, default=ROOT/'build/greenpath-python')
    p.add_argument('--tex3ds', type=Path, default=Path(TEX3DS_EXE))
    p.add_argument('--scale', type=float, default=.25)
    p.add_argument('--romfs', type=Path, default=ROOT/'_private/converted/hero/romfs')
    p.add_argument('--no-romfs-copy', dest='romfs', action='store_const', const=None,
                   help='Leave ROMFS installation to the regeneration manifest.')
    convert(p.parse_args())
