"""Export pickup placement from the private Unity 6000 player copy (no editor).

This small reader handles the built-in GameObject, Transform and BoxCollider2D
layouts used by build 22529139. It never executes MonoBehaviours. Unknown shiny
item FSMs are reported, rather than guessing their reward from the shared prefab.
Run after scene conversion: python tools/pickup_assets.py.
"""
import converted_files  # Install hardlink copy-on-write before asset writes.
import argparse
import hashlib
import json
import math
import struct
from pathlib import Path
from permanent_pickup_assets import SCENES as PERMANENT_SCENES, placements as permanent_placements

ROOT = Path(__file__).resolve().parents[1]
SCENES = {'Tutorial_01': 6, 'Town': 7, 'Crossroads_01': 37,
          'Crossroads_02': 38, 'Crossroads_07': 43, 'Crossroads_11_alt': 50,
          'Crossroads_33': 64, 'Crossroads_39': 69}


class Asset:
    def __init__(self, path):
        self.data = Path(path).read_bytes()
        self.sha256 = hashlib.sha256(self.data).hexdigest()
        if struct.unpack_from('>I', self.data, 8)[0] != 22 or self.data[16] != 0:
            raise ValueError('Expected little-endian serialized file v22')
        self.pos = 48
        end = self.data.index(0, self.pos)
        if not self.data[self.pos:end].startswith(b'6000.'):
            raise ValueError('Unsupported Unity version')
        self.pos = end + 1
        self.read('i')  # target platform
        if self.read('?'):
            raise ValueError('Expected the built player without type trees')
        types = []
        for _ in range(self.read('i')):
            kind = self.read('i')
            self.read('?'); self.read('h')
            if kind == 114 or kind < 0:
                self.pos += 16
            self.pos += 16
            types.append(kind)
        self.objects = {}
        start = struct.unpack_from('>Q', self.data, 32)[0]
        for _ in range(self.read('i')):
            self.pos = (self.pos + 3) // 4 * 4
            owner, offset, size, kind = self.read('qqIi')
            if offset < 0 or start + offset + size > len(self.data):
                raise ValueError('Invalid source object extent')
            self.objects[owner] = (types[kind], self.data[start+offset:start+offset+size])
        self.gos = {}
        self.transforms = {}
        for owner, (kind, data) in self.objects.items():
            if kind == 1:
                count = struct.unpack_from('<i', data)[0]
                components = [struct.unpack_from('<iq', data, 4+i*12)[1] for i in range(count)]
                pos = 8 + count*12
                length = struct.unpack_from('<i', data, pos)[0]
                name = data[pos+4:pos+4+length].decode('utf-8')
                active = data[(pos+4+length+3)//4*4+2] != 0
                self.gos[owner] = dict(name=name, components=components, active=active)
            elif kind == 4:
                go = struct.unpack_from('<iq', data)[1]
                children = struct.unpack_from('<i', data, 52)[0]
                parent = struct.unpack_from('<iq', data, 56+children*12)[1]
                self.transforms[owner] = (go, parent, data)

    def read(self, fmt):
        values = struct.unpack_from('<'+fmt, self.data, self.pos)
        self.pos += struct.calcsize('<'+fmt)
        return values[0] if len(values) == 1 else values

    def transform(self, owner):
        return next(p for p in self.gos[owner]['components'] if self.objects[p][0] == 4)

    def point(self, transform, point):
        _, parent, data = self.transforms[transform]
        qx, qy, qz, qw = struct.unpack_from('<4f', data, 12)
        if abs(qx)+abs(qy) > 1e-5:
            raise ValueError('Tilted pickup transform')
        x, y, _ = struct.unpack_from('<3f', data, 28)
        sx, sy, _ = struct.unpack_from('<3f', data, 40)
        cosine, sine = 1-2*qz*qz, 2*qw*qz
        point = (x+cosine*sx*point[0]-sine*sy*point[1],
                 y+sine*sx*point[0]+cosine*sy*point[1])
        return self.point(parent, point) if parent else point

    def active(self, transform):
        go, parent, _ = self.transforms[transform]
        return self.gos[go]['active'] and (not parent or self.active(parent))

    def descendants(self, transform):
        result = []
        for child, (go, parent, _) in self.transforms.items():
            if parent == transform:
                result.extend([go, *self.descendants(child)])
        return result


def fsm_int(data, name):
    """Read a named int from the variable table at the end of a PlayMaker FSM."""
    token = name.encode('ascii')
    pos = data.rfind(struct.pack('<I', len(token))+token)
    if pos < 0:
        raise ValueError('Missing FSM int '+name)
    pos = (pos+4+len(token)+3)//4*4
    length = struct.unpack_from('<I', data, pos)[0]  # tooltip
    pos = (pos+4+length+3)//4*4
    value = struct.unpack_from('<i', data, pos+8)[0]
    if not 0 <= value <= 10000:
        raise ValueError('Invalid FSM int '+name)
    return value


def placements(asset, scene_number):
    records, skipped = [], []
    for owner, go in asset.gos.items():
        name = go['name']
        if not (name.startswith('Geo Rock') or name.startswith('Shiny Item')):
            continue
        transform = asset.transform(owner)
        if not asset.active(transform):
            continue
        if not name.startswith('Geo Rock'):
            if name.startswith('Shiny Item'):
                skipped.append(dict(owner=owner, name=name, reason='reward FSM not classified'))
            continue
        fsms = [asset.objects[p][1] for p in go['components'] if asset.objects[p][0] == 114]
        fsm = next(b for b in fsms if b'Geo Per Hit' in b)
        hits = fsm_int(fsm, 'Hits')
        value = (hits-1)*fsm_int(fsm, 'Geo Per Hit') + fsm_int(fsm, 'Final Payout')
        points = []
        for component in go['components']:
            kind, data = asset.objects[component]
            if kind != 61 or not data[12]:
                continue
            if len(data) != 148:
                raise ValueError('Unsupported BoxCollider2D layout')
            x, y = struct.unpack_from('<2f', data, 72)
            width, height = struct.unpack_from('<2f', data, 136)
            points.extend(asset.point(transform, (x+dx*width/2, y+dy*height/2))
                          for dx in (-1, 1) for dy in (-1, 1))
        if not points or not hits:
            raise ValueError('Missing rock collider/hits')
        position = asset.point(transform, (0, 0))
        bounds = [min(p[0] for p in points), min(p[1] for p in points),
                  max(p[0] for p in points), max(p[1] for p in points)]
        if not all(math.isfinite(v) for v in (*position, *bounds)):
            raise ValueError('Nonfinite source placement')
        records.append(dict(id=scene_number*1000000+owner, owner=owner, kind=3, hits=hits,
                            value=value, position=position, bounds=bounds,
                            visual_owners=asset.descendants(transform), name=name))
    return records, skipped


def pack(records):
    if len(records) > 128:
        raise ValueError('Pickup bank capacity exceeded')
    ranges = any(r.get('require_hero_range', False) for r in records)
    data = bytearray(struct.pack('<4sI', b'PUP2' if ranges else b'PUP1', len(records)))
    for r in records:
        owners = r['visual_owners']
        if len(owners) > 128:
            raise ValueError('Pickup visual owner capacity exceeded')
        data += struct.pack('<5I6fI', r['id'], r['owner'], r['kind'], r['hits'], r['value'],
                            *r['position'], *r['bounds'], len(owners))
        data += struct.pack('<'+'I'*len(owners), *owners)
        if ranges:
            require_range = r.get('require_hero_range', False)
            if type(require_range) is not bool:
                raise ValueError('Invalid Hero Range flag')
            hero_range = r.get('hero_range', (0, 0, 0, 0)) if require_range else (0, 0, 0, 0)
            if require_range and (r['kind'] != 0 or len(hero_range) != 4 or
                    not all(math.isfinite(v) for v in hero_range) or
                    hero_range[0] >= hero_range[2] or hero_range[1] >= hero_range[3]):
                raise ValueError('Invalid Hero Range bounds')
            data += struct.pack('<I4f', int(require_range), *hero_range)
    return data


def export(source, romfs, scene=None):
    rooms = SCENES | PERMANENT_SCENES
    if scene is not None:
        rooms = {scene: rooms[scene]}
    output = ROOT/'_private/converted/collectibles'
    manifest = output/'manifest.json'
    # Scoped exports preserve banks and evidence owned by other rooms.
    report = json.loads(manifest.read_text()) if scene is not None and manifest.exists() else {}
    for scene, number in rooms.items():
        asset = Asset(source/f'level{number}')
        # Geo owners retain their existing room selection and payout rules.
        records, skipped = placements(asset, number) if scene in SCENES else ([], [])
        records.extend(permanent_placements(asset, number))
        target = romfs if scene == 'Tutorial_01' else romfs/'scenes'/scene
        target.mkdir(parents=True, exist_ok=True)
        (target/'collectibles.bin').write_bytes(pack(records))
        approximations = []
        if any(r['kind'] == 3 for r in records):
            approximations.extend(['Deposit total is awarded on the final strike; partial hits reset on reload.',
                                   'Total assumes Final Payout replaces Geo Per Hit on the final strike.'])
        if any(r['kind'] == 0 for r in records):
            approximations.append('Bottle and captive artwork hide immediately; rescue animation, debris and audio remain absent.')
        report[scene] = dict(source_sha256=asset.sha256, records=records, skipped=skipped,
                            approximations=approximations)
        print(scene, len(records), 'pickups;', len(skipped), 'unclassified shinies')
    output.mkdir(parents=True, exist_ok=True)
    manifest.write_text(json.dumps(report, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT/'_private/original/Hollow Knight/hollow_knight_Data')
    parser.add_argument('--romfs', type=Path, default=ROOT/'_private/converted/hero/romfs')
    parser.add_argument('--scene', choices=SCENES | PERMANENT_SCENES,
                        help='Export one room without changing other pickup banks')
    args = parser.parse_args()
    export(args.source, args.romfs, args.scene)
