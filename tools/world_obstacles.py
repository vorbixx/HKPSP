"""Source-authored static hazard triggers and respawn markers for scene banks.

Uses the same DamageHero/HazardRespawnTrigger records as convert_tutorial.
Unsupported geometry/conditional hazards stay explicit in the scene report.
"""
import json
import hashlib
import math
import struct
from convert_terrain import paths, transform_point
from inspect_fsm import decode_fsm
from acid_hazard_controller import acid_armour_controller
from spike_hazard_controller import spike_slash_controller
from hazard_polygon_geometry import simple_polygon_path


def read_component(root, name):
    path = root / name
    # Optional marker references can be absent from the saved export. Report
    # that source gap without opening an undeclared, nonexistent input.
    if not path.is_file():
        raise FileNotFoundError(f'Missing scene component: {name}')
    return json.loads(path.read_text())


def component_reader(root, supplement=None):
    """Admit a separately recovered marker set without replacing room geometry."""
    if supplement is None:
        return lambda name: read_component(root, name)
    provenance = json.loads((supplement / 'sources.json').read_text(encoding='utf-8'))
    if provenance['terrain_sha256'] != hashlib.sha256((root / 'terrain-map.json').read_bytes()).hexdigest():
        raise ValueError('Hazard marker supplement belongs to a different room export')
    hashes = provenance['source_sha256']
    if not isinstance(hashes, dict) or not hashes:
        raise ValueError('Hazard marker supplement has no source dependencies')
    recovered = {}
    for name, expected in hashes.items():
        if not isinstance(name, str) or '/' in name or '\\' in name or not name.endswith('.json'):
            raise ValueError('Invalid hazard marker source filename')
        raw = (supplement / name).read_bytes()
        if hashlib.sha256(raw).hexdigest() != expected:
            raise ValueError(f'Changed hazard marker source: {name}')
        recovered[name] = json.loads(raw)

    def read(name):
        if (root / name).is_file() or name not in recovered:
            return read_component(root, name)
        return recovered[name]
    return read



def static_hazard_fsm(read, name, acid=False, spike=False):
    """Recognize source controllers modeled by static spike/acid records.

    Build22529139 level135-6462 only inspects the contacted actor and forwards
    damage/events. None of these actions moves or enables the spike collider.
    Unknown FSMs/actions still require their own lifetime adapter.
    """
    try:
        component = read(name)
    except (KeyError, FileNotFoundError):
        return False
    fsm = component.get('m_Structure', {}).get('fsm', {})
    if fsm.get('name') == 'Slash Reaction':
        try:
            return spike and name == 'level134-6184.json' and spike_slash_controller(decode_fsm(fsm))
        except (KeyError, ValueError, TypeError):
            return False
    if fsm.get('name') == 'Acid Armour Check':
        try:
            return acid and acid_armour_controller(decode_fsm(fsm))
        except (KeyError, ValueError, TypeError):
            return False
    if fsm.get('name') not in ('damages_enemy', 'enemy_message'):
        return False
    allowed = {'CheckSendEventLimit', *('HutongGames.PlayMaker.Actions.' + name for name in (
        'Collision2dEvent', 'Trigger2dEvent', 'IntCompare', 'GetLayer', 'GetName',
        'CompareNames', 'SendEventByName', 'TakeDamage', 'SendEvent', 'SendEnemyMessage',
        'GetParent', 'GameObjectIsNull'))}
    states = fsm.get('states', [])
    return bool(states) and all(state.get('actionData', {}).get('actionNames') and
        all(action in allowed for action in state['actionData']['actionNames']) for state in states)


def collect(inventory, read):
    hazards, checkpoints, unsupported = [], [], []
    for row in inventory:
        local = row['components'][0]
        if not all(read(owner)['m_IsActive'] for owner in row['owners']):
            continue
        collider = read(row['collider'])
        if not collider.get('m_Enabled', True):
            continue
        relevant = set(local) & {'DamageHero', 'HazardRespawnTrigger'}
        if not relevant:
            continue
        # Enemy contact and moving stalactites have their own actor adapters.
        if any(set(group) & {'HealthManager', 'StalactiteControl'} for group in row['components']):
            continue
        # Initial transforms/enabled bits do not describe a moving or scripted
        # hazard's lifetime. Leave those to a source-specific controller.
        controllers = {'Rigidbody2D', 'PersistentBoolItem', 'PersistentIntItem',
                       'DeactivateIfPlayerdataTrue', 'ActivateChildrenOnContact', 'PlayerDataBoolTest'}
        damage = read(local['DamageHero'][0]) if 'DamageHero' in local else {}
        acid = (damage.get('m_Enabled', True) and
                damage.get('m_Structure', {}).get('hazardType') == 3)
        spike = (damage.get('m_Enabled', True) and
                 damage.get('m_Structure', {}).get('hazardType') == 2)
        controlled = any(set(group) & controllers for group in row['components']) or any(
            not static_hazard_fsm(read, name, acid=acid and group is local,
                                  spike=spike and group is local) for group in row['components']
            for name in group.get('PlayMakerFSM', []))
        def rectangle():
            if row['type'] != 'BoxCollider2D' or not collider.get('m_IsTrigger', False):
                raise ValueError('requires an axis-aligned box trigger')
            chain = [read(key) for key in row['chain']]
            points = [transform_point(p, chain) for path in paths(collider, row['type']) for p in path]
            xs = sorted({round(p[0], 5) for p in points})
            ys = sorted({round(p[1], 5) for p in points})
            if len(xs) != 2 or len(ys) != 2:
                raise ValueError('requires an axis-aligned box trigger')
            return [min(p[0] for p in points), min(p[1] for p in points),
                    max(p[0] for p in points), max(p[1] for p in points)]
        for kind in sorted(relevant):
            component = read(local[kind][0])
            if not component.get('m_Enabled', True):
                continue
            try:
                data = component['m_Structure']
                if controlled:
                    raise ValueError('moving/scripted/persistent trigger needs a dedicated adapter')
                if kind == 'DamageHero':
                    if data['hazardType'] == 1 or data['damageDealt'] == 0:
                        continue
                    if data['hazardType'] not in (2, 3):
                        raise ValueError('hazard type/immunity needs a dedicated adapter')
                    polygons = []
                    if row['type'] == 'PolygonCollider2D' and collider.get('m_IsTrigger', False):
                        chain = [read(key) for key in row['chain']]
                        polygons = [[transform_point(p, chain) for p in path]
                                    for path in paths(collider, row['type'])]
                        if not 0 < len(polygons) <= 8 or any(not 3 <= len(path) <= 64 for path in polygons):
                            raise ValueError('hazard polygon exceeds bounded path capacity')
                        for path in polygons:
                            if not simple_polygon_path(path):
                                raise ValueError('self-crossing/invalid hazard polygon')
                            if abs(sum(a[0]*b[1]-b[0]*a[1] for a,b in zip(path, path[1:]+path[:1]))) <= 1e-8:
                                raise ValueError('degenerate hazard polygon')
                        points = [p for path in polygons for p in path]
                        rect = [min(p[0] for p in points), min(p[1] for p in points),
                                max(p[0] for p in points), max(p[1] for p in points)]
                    else:
                        rect = rectangle()
                    hazard = dict(rect=rect, damage=data['damageDealt'], type=data['hazardType'],
                                  shadow_dash=bool(data.get('shadowDashHazard', False)), source=local[kind][0])
                    if polygons:
                        hazard['polygons'] = polygons
                    hazards.append(hazard)
                else:
                    prefix = row['go'].rsplit('-', 1)[0]
                    def reference(pointer):
                        if pointer.get('m_FileID', 0) or not pointer['m_PathID']:
                            raise ValueError('respawn reference is not local')
                        return f"{prefix}-{pointer['m_PathID']}.json"
                    marker = read(reference(data['respawnMarker']))
                    owner = read(reference(marker['m_GameObject']))
                    components = [read(reference(p['m_Component'])) for p in owner['m_Components']]
                    chain = [next(p for p in components if 'm_LocalPosition' in p)]
                    while chain[-1]['m_Father']['m_PathID']:
                        if len(chain) >= 64:
                            raise ValueError('cyclic/too deep marker hierarchy')
                        chain.append(read(reference(chain[-1]['m_Father'])))
                    checkpoints.append(dict(rect=rectangle(), position=transform_point([0, 0], chain),
                                            facing_right=marker['m_Structure']['respawnFacingRight'],
                                            fire_once=data['fireOnce'], source=local[kind][0]))
            except (ValueError, FileNotFoundError, KeyError, StopIteration) as error:
                unsupported.append(dict(owner=row['go'], kind=kind, reason=str(error)))
    if len(hazards) > 128 or len(checkpoints) > 128:
        raise ValueError('world obstacle bank exceeds bounded runtime capacity')
    return dict(hazards=hazards, checkpoints=checkpoints, unsupported=unsupported)


def export(inventory, read, target, output):
    report = collect(inventory, read)
    # Flagged records opt into HKO2; unchanged scenes retain byte-identical HKO1.
    polygon_bank = any(h.get('polygons') for h in report['hazards'])
    magic = b'HKO3' if polygon_bank else b'HKO2' if any(h['shadow_dash'] for h in report['hazards']) else b'HKO1'
    blob = bytearray(struct.pack('<4sII', magic, len(report['hazards']), len(report['checkpoints'])))
    for hazard in report['hazards']:
        if not 0 < hazard['damage'] <= 100:
            raise ValueError('invalid hazard damage')
        blob += struct.pack('<4fII', *hazard['rect'], hazard['damage'],
                            hazard['type'] | (0x100 if hazard['shadow_dash'] else 0))
    for checkpoint in report['checkpoints']:
        flags = int(checkpoint['facing_right']) | int(checkpoint['fire_once']) << 1
        blob += struct.pack('<6fI', *checkpoint['rect'], *checkpoint['position'], flags)
    total_points = sum(len(path) for h in report['hazards'] for path in h.get('polygons', []))
    if total_points > 4096:
        raise ValueError('hazard polygon bank exceeds bounded vertex capacity')
    if polygon_bank:
        for hazard in report['hazards']:
            polygons = hazard.get('polygons', [])
            blob += struct.pack('<I', len(polygons))
            for path in polygons:
                blob += struct.pack('<I', len(path))
                for point in path:
                    if not all(math.isfinite(v) and abs(v) <= 100000 for v in point):
                        raise ValueError('invalid obstacle polygon coordinates')
                    blob += struct.pack('<2f', *point)
    if not all(math.isfinite(v) and abs(v) <= 100000 for record in (*report['hazards'], *report['checkpoints'])
               for v in (*record['rect'], *record.get('position', []))):
        raise ValueError('invalid obstacle coordinates')
    (target/'world-obstacles.bin').write_bytes(blob)
    (output/'world-obstacles.json').write_text(json.dumps(report, indent=2)+'\n')
    return report
