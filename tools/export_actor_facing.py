"""Export authored facing evidence from private snapshots, without running Unity.

The additional scene components repair the old battle/Greenpath snapshot gaps.
Inputs are read-only. The JSON is a converted configuration, not a table of
inferred species defaults: absent or conflicting evidence remains an error.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import struct
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from inspect_fsm import decode_fsm

ROOT = Path(__file__).resolve().parents[1]

# Authored owners, not artwork-library holders (which have no actor controller).
# Scene and sharedassets numbers are independent in the original build.
COMPONENT_OWNERS = {
    'enemy-b/goam': ('level52', 177),
    'uumuu-body/Uumuu': ('level229', 15),
    'enemy-c/baldur': ('level76', 261),
    'dung-defender/Dung': ('level355', 10),
    'stag/stag': ('level9', 255),
    'elder-baldur/': ('level50', 729),
    'elder-baldur/Roller': ('sharedassets50.assets', 59),
    'false-knight/Knight': ('level48', 40),
    'false-knight/Head': ('level48', 48),
    'crossroads-bosses/Head': ('level48', 48),
    'broken-vessel/Scuttler': ('sharedassets343.assets', 37),
    'broken-vessel/Balloon': ('sharedassets343.assets', 43),
    'monomon-guardian-art/Monomon': ('level402', 84),
    'world-quest-art/sly': ('level20', 26),
    'bretta-quest-art/bretta': ('level187', 191),
    'shade/shade': ('resources.assets', 4541),
}


def source_root():
    return Path(os.environ.get('HKPSP_SOURCE_ROOT', next((p / 'HK3DS' for p in ROOT.parents if (p / 'HK3DS').is_dir()), ROOT.parent / 'HK3DS')))


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def structure(data):
    return data.get('m_Structure', data)


def literal(value, variables=None):
    if not isinstance(value, dict):
        return value
    if 'variable' in value:
        return (variables or {}).get(value['variable'])
    if value.get('useVariable'):
        return (variables or {}).get(value.get('name'))
    return value.get('value')


def variable_name(value):
    if not isinstance(value, dict):
        return None
    return value.get('variable') or (value.get('name') if value.get('useVariable') else None)


def owner_action(parameters):
    """Exclude scale/facing actions on effects, weapons and other actors."""
    target = parameters.get('gameObject')
    return target is None or isinstance(target, dict) and target.get('ownerOption') == 0


def locomotion_scales(fsm):
    """Trace the audited locomotion controllers, including their variable writes.

    This deliberately excludes combat strafing/retreat controllers. State names
    are only selectors; signed movement and scale values must prove both branches.
    """
    contracts = {
        'Acid Walker': ('Init', ('Left', 'Right'), 'Walk Speed Crt', 'Start Walk'),
        'Mossy Control': (None, ('Emerge Right', 'Emerge Left'), 'Current Charge Speed', 'Charge'),
        'Orange Scuttler': ('Init', ('Run L', 'Run R'), 'Velocity', None),
    }
    contract = contracts.get(fsm.get('name'))
    if not contract:
        return []
    states = {s['name']: s for s in fsm['states']}
    prelude, branches, speed, consumer = contract
    if any(s not in states for s in (*branches, *([prelude] if prelude else []), *([consumer] if consumer else []))):
        return []
    initial = {v['name']: v.get('value') for pool in fsm.get('variables', {}).values()
               if isinstance(pool, list) for v in pool if isinstance(v, dict) and 'name' in v}

    def actions(state):
        return [(i, a['type'].split('.')[-1], a['parameters'])
                for i, a in enumerate(states[state].get('actions', [])) if a.get('enabled', True)]

    def trace(state, values):
        for _, kind, p in actions(state):
            dest = variable_name(p.get('floatVariable', p.get('storeResult', {})))
            if kind == 'RandomFloat':
                lo, hi = literal(p.get('min'), values), literal(p.get('max'), values)
                # Any sample in this interval must have the same nonzero sign.
                values[dest] = lo if isinstance(lo, (int, float)) and isinstance(hi, (int, float)) and lo*hi > 0 else None
            elif kind == 'SetFloatValue':
                values[dest] = literal(p.get('floatValue'), values)
            elif kind == 'FloatMultiply':
                value, factor = values.get(dest), literal(p.get('multiplyBy'), values)
                values[dest] = value*factor if isinstance(value, (int, float)) and isinstance(factor, (int, float)) else None

    if prelude:
        trace(prelude, initial)
    results = []
    for branch in branches:
        values = initial.copy()
        trace(branch, values)
        scales = [(i, literal(p.get('x'), values)) for i, k, p in actions(branch)
                  if k == 'SetScale' and owner_action(p)]
        if len(scales) != 1:
            return []
        index, scale = scales[0]
        if consumer:
            reads = [p for _, k, p in actions(consumer) if k == 'SetVelocity2d' and owner_action(p)
                     and variable_name(p.get('x')) == speed]
            motion = values.get(speed) if len(reads) == 1 else None
        else:
            reads = [p for _, k, p in actions(branch) if k == 'SetVelocity2d' and owner_action(p)
                     and variable_name(p.get('x')) == speed]
            gets = [p for _, k, p in actions(branch) if k == 'GetVelocity2d' and owner_action(p)
                    and variable_name(p.get('x')) == speed]
            adds = [literal(p.get('add', p.get('subtract')), values) * (-1 if k == 'FloatSubtract' else 1)
                    for _, k, p in actions(branch) if k in ('FloatAdd', 'FloatSubtract')
                    and variable_name(p.get('floatVariable')) == speed]
            motion = adds[0] if len(reads) == len(gets) == len(adds) == 1 else None
        if not all(isinstance(v, (int, float)) and math.isfinite(v) and v != 0 for v in (scale, motion)):
            return []
        results.append((branch, index, scale, motion))
    if results[0][2]*results[1][2] >= 0 or results[0][3]*results[1][3] >= 0:
        return []
    return results


def moss_walker_scale(fsm):
    """Follow GetScale -> signed branch -> floor vector -> Walking consumer."""
    if fsm.get('name') != 'Moss Walker':
        return None
    states = {s['name']: s for s in fsm['states']}
    def params(state, kind):
        return [a['parameters'] for a in states.get(state, {}).get('actions', [])
                if a.get('enabled', True) and a['type'].split('.')[-1] == kind]
    gets, compares = params('Check Dir', 'GetScale'), params('Check Dir', 'FloatCompare')
    if len(gets) != 1 or len(compares) != 1 or not owner_action(gets[0]):
        return None
    compare = compares[0]
    if compare.get('float1') != gets[0].get('xScale') or literal(compare.get('float2')) != 0:
        return None
    transitions = {t['fsmEvent']['name']: t['toState'] for t in states['Check Dir'].get('transitions', [])}
    results = []
    for event, scale in ((compare.get('lessThan'), -1), (compare.get('greaterThan'), 1)):
        copies = params(transitions.get(event), 'SetVector2XY')
        if len(copies) != 1:
            return None
        vectors = [p for p in params('Floor', 'SetVector2XY') if p.get('vector2Variable') == copies[0].get('vector2Value')]
        consumers = [p for p in params('Walk Start', 'SetVelocity2d')
                     if owner_action(p) and p.get('vector') == copies[0].get('vector2Variable')]
        if len(vectors) != 1 or len(consumers) != 1:
            return None
        motion = literal(vectors[0].get('x'))
        if not isinstance(motion, (int, float)) or motion == 0:
            return None
        results.append(scale * (1 if motion > 0 else -1))
    return results[0] if results[0] == results[1] else None


def pigeon_scale(fsm):
    """Maskfly faces its force vector; the left branch flips sprite and angle."""
    if fsm.get('name') != 'Pigeon':
        return None
    branches = []
    for name in ('Right', 'Left'):
        state = next((s for s in fsm['states'] if s['name'] == name), {})
        actions = {a['type'].split('.')[-1]: a['parameters'] for a in state.get('actions', []) if a.get('enabled', True)}
        random, force, angle, scale = [actions.get(k, {}) for k in ('RandomFloat', 'AddForce2d', 'FaceAngle', 'SetScale')]
        lo, hi = literal(random.get('min')), literal(random.get('max'))
        if (not all(isinstance(v, (int, float)) for v in (lo, hi)) or lo*hi <= 0
                or force.get('x') != random.get('storeResult') or not owner_action(force)
                or not owner_action(angle) or not owner_action(scale) or literal(scale.get('x')) != 1):
            return None
        sprite = actions.get('Tk2dSpriteSetScale', {})
        if name == 'Right':
            if lo <= 0 or literal(angle.get('angleOffset')) != 0 or sprite:
                return None
        elif hi >= 0 or literal(angle.get('angleOffset')) != 180 or not owner_action(sprite) or (literal(sprite.get('scale')) or {}).get('x') != -1:
            return None
        branches.append(name)
    return 1 if len(branches) == 2 else None


def facing_bool_scales(fsm):
    """Paired, explicit facing flags and owner scale writes, never velocity guesses.

    Original Brawler and FalseyControl both write Facing Right on each turn.
    Require both polarities and opposite scale signs; an isolated state or an
    effect-target scale is insufficient evidence.
    """
    pairs = []
    for state in fsm['states']:
        enabled = [(i, a) for i, a in enumerate(state.get('actions', [])) if a.get('enabled', True)]
        flags = [(i, literal(a['parameters'].get('boolValue')))
                 for i, a in enabled if a['type'].split('.')[-1] == 'SetBoolValue'
                 and variable_name(a['parameters'].get('boolVariable')) in
                 (('Attacking R',) if fsm.get('name') == 'Mantis Lord' else ('Facing Right',))]
        scales = [(i, literal(a['parameters'].get('x')))
                  for i, a in enabled if a['type'].split('.')[-1] == 'SetScale'
                  and owner_action(a['parameters'])]
        if len(flags) == len(scales) == 1:
            _, flag = flags[0]
            index, scale = scales[0]
            if isinstance(flag, bool) and isinstance(scale, (float, int)) and scale != 0:
                pairs.append((state['name'], index, flag, scale))
    if {p[2] for p in pairs} != {False, True}:
        return []
    signs = {(1 if scale > 0 else -1) * (1 if flag else -1) for _, _, flag, scale in pairs}
    return pairs if len(signs) == 1 else []


def exported_collections(scenes):
    """Map level collections to scene exports from their terrain inventories.

    2026-11-25: Steam builds export rooms without the HK3DS batch indexes, so
    no boss owner (Hornet, Mantis Lords, Watchers) was found. Each exported
    terrain object is named `level<N>-<path id>.json` after its collection.
    """
    found = {}
    for scene in scenes:
        path = scene / 'terrain/terrain-map.json'
        if not path.is_file():
            continue
        levels = {str(row.get('go', '')).split('-', 1)[0] for row in read_json(path).get('inventory', [])}
        levels = {level for level in levels if level.startswith('level') and level[5:].isdigit()}
        if len(levels) != 1:
            raise ValueError(f'{path}: expected one level collection, found {sorted(levels)}')
        found.setdefault(levels.pop(), []).append(scene)
    return found


class Snapshots:
    def __init__(self, root):
        self.root = Path(root) / '_private/research'
        self.scenes = sorted((self.root / 'crossroads-scenes').glob('*'))
        self.locations = {}
        self.collection_scenes = {}
        for name in ('batch-1-input.json', 'batch3-source.json', 'export-all-scenes-status.json'):
            path = self.root/'crossroads-scenes'/name
            if path.exists():
                data = read_json(path)
                for key in ('scenes', 'selected'):
                    for scene, level in (data.get(key, {}) if isinstance(data.get(key), dict) else {}).items():
                        if isinstance(level, int):
                            self.collection_scenes.setdefault(f'level{level}', []).append(self.root/'crossroads-scenes'/scene)
        if not self.collection_scenes:
            self.collection_scenes = exported_collections(self.scenes)

    def locate(self, name, preferred=None):
        key = (name, str(preferred))
        if key not in self.locations:
            candidates = ([preferred / name] if preferred else [])
            collection = name.rsplit('-', 1)[0]
            candidates += [scene/sub/name for scene in self.collection_scenes.get(collection, [])
                           for sub in ('terrain', 'visuals', '')]
            # Exact filenames, never a recursive scan of the multi-million-file tree.
            candidates += [scene / sub / name for scene in self.scenes
                           for sub in ('terrain', 'visuals', '')]
            self.locations[key] = next((p for p in candidates if p.is_file()), None)
        return self.locations[key]

    def named(self, collection, names):
        owners = {}
        for scene in self.collection_scenes.get(collection, []):
            for sub, manifest in [('terrain', 'terrain-map.json'), ('visuals', 'room-visuals-map.json')]:
                directory = scene/sub
                path = directory/manifest
                if not path.exists():
                    continue
                for row in read_json(path).get('inventory', []):
                    for name in [row.get('go'), *row.get('owners', [])]:
                        if not name or not name.startswith(collection + '-') or name in owners:
                            continue
                        path = directory/name
                        if path.exists():
                            owners[name] = self.component(path).get('m_Name', '')
        matches = [name for name, label in owners.items() if label in names]
        if not matches:
            return []
        # Multiple scene instances are audited independently by callers when
        # they affect facing; a single kind's convention uses its first root.
        identity = int(sorted(matches)[0].rsplit('-', 1)[1].removesuffix('.json'))
        return self.actor(collection, identity)

    def component(self, path):
        data = read_json(path)
        body = structure(data)
        # AssetRipper moves MonoBehaviour headers outside m_Structure.
        return {**{k: v for k, v in data.items() if k != 'm_Structure'}, **body,
                '_source_pointer': '/m_Structure' if 'm_Structure' in data else ''}

    def actor(self, collection, identity, preferred=None):
        path = self.locate(f'{collection}-{identity}.json', preferred)
        if path is None:
            return []
        go = self.component(path)
        components = []
        for item in go.get('m_Component', go.get('m_Components', [])):
            ref = item.get('component', item.get('m_Component'))
            if ref['m_FileID']:
                raise ValueError(f'{path}: external actor component requires an exported dependency')
            p = self.locate(f'{collection}-{ref["m_PathID"]}.json', path.parent)
            if p is None:
                raise ValueError(f'{path}: component {ref["m_PathID"]} was not exported')
            components.append((p, self.component(p)))
        return components

    def parts(self, directory, parts):
        result = []
        for names in parts.values():
            for name in names:
                p = self.locate(name, directory)
                if p is not None:
                    result.append((p, self.component(p)))
        return result


def facing_from_components(components):
    evidence, signs, instance, instance_evidence = [], [], None, None
    references, fsm_count = set(), 0

    def add(path, pointer, value, right):
        if not isinstance(right, (int, float)) or not math.isfinite(right) or right == 0:
            raise ValueError(f'{path}:{pointer}: invalid facing {right!r}')
        signs.append(1 if right > 0 else -1)
        evidence.append(dict(file=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                             pointer=pointer, value=value, right_sign=signs[-1]))

    for path, data in components:
        prefix = data.get('_source_pointer', '')
        # Native Walker/Crawler animation fields are references too. A missing
        # FSM must not erase the fields that were actually exported.
        for key, value in data.items():
            if isinstance(value, str) and value and ('anim' in key.lower() or 'clip' in key.lower()):
                references.add(value)
        if 'm_LocalScale' in data:
            scale = data['m_LocalScale']
            value = scale.get('x', scale.get('m_X'))
            if instance is not None and value != instance:
                raise ValueError('Several root transforms in one actor')
            instance = value
            instance_evidence = dict(file=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                                     pointer=prefix+'/m_LocalScale', value=scale)
        if 'rightScale' in data:
            add(path, prefix+'/rightScale', data['rightScale'], data['rightScale'])
        if 'spriteFacesLeft' in data:
            if not isinstance(data['spriteFacesLeft'], bool):
                raise ValueError(f'{path}: spriteFacesLeft is not an authored boolean')
            add(path, prefix+'/spriteFacesLeft', data['spriteFacesLeft'], -1 if data['spriteFacesLeft'] else 1)
        # Climber.Start (build 22529139): clockwise is sign(localScale.x),
        # inverted when !startRight; at rotation 0 clockwise walks right.
        # Require the controller's complete serialized field signature, rather
        # than interpreting an unrelated startRight field as a convention.
        if {'startRight', 'speed', 'spinTime', 'wallRayPadding', 'constrain', 'minTurnDistance'} <= data.keys():
            if not isinstance(data['startRight'], bool):
                raise ValueError(f'{path}: Climber.startRight is not an authored boolean')
            add(path, prefix+'/startRight', data['startRight'], 1 if data['startRight'] else -1)
        fsm = data.get('fsm')
        if not fsm or 'states' not in fsm:
            continue
        fsm_count += 1
        decoded = decode_fsm(fsm) if 'actionData' in str(fsm['states'][:1]) else fsm
        variables = {v['name']: v.get('value') for pool in decoded.get('variables', {}).values()
                     if isinstance(pool, list) for v in pool if isinstance(v, dict) and 'name' in v}
        owner_variables = {
            variable_name(action['parameters'].get('storeGameObject'))
            for state in decoded['states'] for action in state.get('actions', [])
            if action.get('enabled', True) and action['type'].split('.')[-1] == 'GetOwner'} - {None, ''}

        def faces_owner(params):
            if 'objectA' not in params:
                return owner_action(params)
            target = params['objectA']
            if variable_name(target) in owner_variables:
                return True
            resolved = literal(target, variables)
            owner = data.get('m_GameObject')
            return isinstance(resolved, dict) and resolved.get('m_PathID', 0) != 0 and resolved == owner
        for state_name, index, flag, scale in facing_bool_scales(decoded):
            pointer = data.get('_fsm_pointer', prefix+'/fsm')+f'/{decoded.get("name", "")}/{state_name}/{index}/SetScale/x'
            add(path, pointer, dict(scale=scale, facing_right=flag), scale if flag else -scale)
        for state_name, index, scale, motion in locomotion_scales(decoded):
            pointer = data.get('_fsm_pointer', prefix+'/fsm')+f'/{decoded.get("name", "")}/{state_name}/{index}'
            add(path, pointer, dict(scale=scale, locomotion=motion), scale if motion > 0 else -scale)
        moss_scale = moss_walker_scale(decoded)
        if moss_scale is not None:
            add(path, data.get('_fsm_pointer', prefix+'/fsm')+'/Moss Walker/Check Dir',
                dict(trace='GetScale/FloatCompare -> floor vector -> Walking'), moss_scale)
        pigeon = pigeon_scale(decoded)
        if pigeon is not None:
            add(path, data.get('_fsm_pointer', prefix+'/fsm')+'/Pigeon/Right+Left',
                dict(trace='signed force -> FaceAngle + sprite scale'), pigeon)
        for state in decoded['states']:
            for index, action in enumerate(state.get('actions', [])):
                if not action.get('enabled', True):
                    continue
                kind, params = action['type'].split('.')[-1], action['parameters']
                base = data.get('_fsm_pointer', prefix+'/fsm')+f'/{decoded.get("name", "")}/{state["name"]}/{index}/{kind}'
                if kind in ('FaceDirection', 'FaceObject', 'FaceObjectV2') and faces_owner(params) and 'spriteFacesRight' in params:
                    value = literal(params['spriteFacesRight'], variables)
                    if isinstance(value, bool):
                        add(path, base + '/spriteFacesRight', value, 1 if value else -1)
                    if literal(params.get('playNewAnimation'), variables) is True:
                        clip = literal(params.get('newAnimationClip'), variables)
                        if isinstance(clip, str) and clip:
                            references.add(clip)
                if 'spriteFacesLeft' in params and owner_action(params):
                    value = literal(params['spriteFacesLeft'], variables)
                    if isinstance(value, bool):
                        add(path, base + '/spriteFacesLeft', value, -1 if value else 1)
                if 'Tk2d' in kind or 'tk2d' in kind:
                    for k, v in params.items():
                        if k.lower() in ('clipname', 'animname', 'animationname', 'clip', 'animation'):
                            value = literal(v, variables)
                            if isinstance(value, str) and value:
                                references.add(value)
        # Scale/velocity pairs alone do not prove facing: attacks can retreat,
        # and floor crawlers rotate. Preserve these as converter gaps until
        # their control flow and target transforms have been verified.
    unique = set(signs)
    verdict = 'ok' if len(unique) == 1 else 'conflicting source data' if unique else 'no source data'
    return dict(right_sign=next(iter(unique)) if len(unique) == 1 else None,
                instance_scale=instance, instance_evidence=instance_evidence, evidence=evidence, verdict=verdict,
                referenced_clips=sorted(references), fsm_count=fsm_count)


def export(root, actors, output):
    snapshots, result, complete = Snapshots(root), {}, {}
    for actor in actors:
        components = []
        entries = actor.get('components', [])
        extended = output.parent/'source-components'/(actor['id'].replace('/', '__')+'.json')
        if extended.exists():
            snapshot = read_json(extended)
            entries = [dict(snapshot=str(extended), identity=snapshot['actors'][0]['path_id'])]
            if snapshot.get('complete_owner'):
                complete[actor['id']] = (extended, snapshot)
        for entry in entries:
            if 'snapshot' in entry:
                path = Path(entry['snapshot'])
                if not path.exists():
                    continue
                for i, owner in enumerate(read_json(path)['actors']):
                    if owner['path_id'] != entry['identity']:
                        continue
                    for j, component in enumerate(owner['components']):
                        base = f'/actors/{i}/components/{j}'
                        body = dict(structure(component.get('data', {})))
                        body['_source_pointer'] = base+'/data'
                        body['_component_id'] = component['path_id']
                        if component.get('decoded'):
                            body['fsm'] = component['decoded']
                            body['_fsm_pointer'] = base+'/decoded'
                        components.append((path, body))
            elif 'parts' in entry:
                components += snapshots.parts(Path(entry['directory']), entry['parts'])
            elif 'names' in entry:
                components += snapshots.named(entry['collection'], entry['names'])
            elif 'collection' in entry:
                components += snapshots.actor(entry['collection'], entry['identity'],
                                              Path(entry['directory']) if entry.get('directory') else None)
        # De-duplicate identical component references from overlapping snapshots.
        components = list({(str(p), d.get('_component_id')): (p, d) for p, d in components}.values())
        try:
            result[actor['id']] = facing_from_components(components)
        except (KeyError, ValueError) as error:
            raise ValueError(f'{actor["id"]}: {error}') from error
    complete_presentations(result, complete)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(dict(version=1, actors=result), indent=2) + '\n', encoding='utf-8')
    records = []
    for kind, facing in sorted(result.items()):
        name = kind.encode('utf-8')
        if len(name) >= 64 or b'\0' in name:
            raise ValueError(f'Facing kind exceeds AFC1 name capacity: {kind}')
        # Zero deliberately makes a partial source export unloadable at runtime.
        records.append(struct.pack('<64si', name, facing['right_sign'] or 0))
    output.with_suffix('.bin').write_bytes(b'AFC1' + struct.pack('<I', len(records)) + b''.join(records))
    return result


def complete_presentations(result, snapshots):
    """Audited fixed/inherited actors have no horizontal facing state machine.

    Instance scale is used only after verifying a complete, identity-selected
    controller export. It is never used as a directional species convention.
    """
    fixed = {'enemy-b/goam': 'Worm Control', 'elder-baldur/': 'Blocker Control',
             'uumuu-body/Uumuu': 'Mega Jellyfish', 'monomon-guardian-art/Monomon': 'Control',
             'route-tolls/city-toll-actor': 'Toll Machine',
             'route-tolls/mines-toll-actor': 'Toll Machine',
             'greenpath-tolls/greenpath-toll-actor': 'Toll Machine'}
    # Audited native components of these owners. A new native controller must
    # be inspected, even if its serialized fields do not mention facing.
    native_classes = set(('tk2dSprite tk2dSpriteAnimator PlayMakerFSM PlayMakerFixedUpdate '
        'PlayMakerCollisionEnter2D SpriteFlash PersonalObjectPool SetZ Recoil DamageHero '
        'HealthManager EnemyDeathEffects InfectedEnemyEffects EnemyDreamnailReaction '
        'DeactivateIfPlayerdataTrue ConstrainPosition ObjectBounce BounceShroom '
        'PersistentBoolItem ExtraDamageable TinkEffect').split())
    for kind, controller in fixed.items():
        if kind not in snapshots or result[kind]['verdict'] != 'no source data':
            continue
        path, snapshot = snapshots[kind]
        components = snapshot['actors'][0]['components']
        if any(c.get('script_class') not in native_classes for c in components if c.get('native_type') == 'MonoBehaviour'):
            continue
        fsms = [c['decoded'] for c in components if c.get('decoded')]
        if controller not in {f.get('name') for f in fsms}:
            continue
        writes = []
        for fsm in fsms:
            owner_vars = {variable_name(a['parameters'].get('storeGameObject'))
                          for s in fsm['states'] for a in s.get('actions', [])
                          if a.get('enabled', True) and a['type'].split('.')[-1] == 'GetOwner'} - {None, ''}
            for s in fsm['states']:
                for a in s.get('actions', []):
                    p, action = a['parameters'], a['type'].split('.')[-1]
                    target = p.get('gameObject', {}).get('gameObject', {})
                    owner = owner_action(p) or variable_name(target) in owner_vars
                    if a.get('enabled', True) and owner and action in (
                            'SetScale', 'FlipScale', 'Tk2dSpriteSetScale', 'FaceObject', 'FaceDirection', 'FaceObjectV2'):
                        writes.append(action)
        scale = result[kind]['instance_scale']
        if writes or not isinstance(scale, (int, float)) or not math.isfinite(scale) or scale == 0:
            continue
        result[kind].update(right_sign=1 if scale > 0 else -1, verdict='ok', presentation='fixed')
        result[kind]['evidence'].append(dict(file=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
            pointer='/actors/0/components', value=dict(controller=controller, complete_owner=True,
                rule='fixed local presentation; no owner-facing writes', scale=scale), right_sign=result[kind]['right_sign']))
    for kind in ('false-knight/Head', 'crossroads-bosses/Head'):
        if kind not in snapshots or result[kind]['verdict'] != 'no source data':
            continue
        path, snapshot = snapshots[kind]
        parent = result.get(kind.replace('/Head', '/Knight'), {})
        ancestors = snapshot.get('ancestors', [])
        if (not ancestors or ancestors[0]['collection'] != 'level48'
                or ancestors[0]['data'].get('m_GameObject') != dict(m_FileID=0, m_PathID=40)
                or result[kind]['instance_scale'] != 1 or parent.get('verdict') != 'ok'):
            continue
        result[kind].update(right_sign=parent['right_sign'], verdict='ok', presentation='inherited')
        result[kind]['evidence'] = [*parent['evidence'], dict(file=str(path),
            sha256=hashlib.sha256(path.read_bytes()).hexdigest(), pointer='/ancestors/0',
            value=dict(rule='unit child inherits False Knight root facing'), right_sign=parent['right_sign'])]


def write_header(result, output):
    """Embed the same converted config in both platforms; no frame-time I/O."""
    if not result or any(r['verdict'] != 'ok' for r in result.values()):
        raise ValueError('Refusing to replace runtime config with incomplete facing evidence')
    lines = ['// Generated by tools/export_actor_facing.py --header; do not edit.',
             '#pragma once', 'namespace hk3ds::actorfacingdata {',
             'struct Entry { const char* kind; int rightSign; bool fixed; };',
             'inline constexpr Entry entries[] = {']
    for kind, facing in sorted(result.items()):
        lines.append('    {%s, %d, %s},' % (json.dumps(kind), facing['right_sign'],
                     'true' if facing.get('presentation') == 'fixed' else 'false'))
    lines += ['};', '}']
    output.parent.mkdir(parents=True, exist_ok=True)
    # A tracked header in the build: rewriting identical content (as CRLF on Windows) made every regen
    # stale the EBOOT (2026-11-19). Write LF, and only when the content changed.
    text = '\n'.join(lines)+'\n'
    if not output.exists() or output.read_text(encoding='utf-8').replace('\r\n', '\n') != text:
        output.write_text(text, encoding='utf-8', newline='\n')


def export_original_components(root, output):
    """Extend incomplete research with complete, identity-selected original owners.

    Output is local private converted data. Original files and HK3DS snapshots
    remain read-only. Each component retains its collection/path ID and hash.
    """
    sys.path.insert(0, str(Path(root)/'_private/tools/boss-python'))
    import UnityPy
    from watcher_unity import AlignedGenerator
    source = Path(root)/'_private/original/Hollow Knight/hollow_knight_Data'
    env = UnityPy.Environment(path=str(source))
    generator = AlignedGenerator('6000.0.61f1')
    generator.load_local_dll_folder(str(source/'Managed'))
    env.typetree_generator = generator
    files, hashes = {}, {}

    def load(name):
        if name not in files:
            env.load_file(str(source/name))
            files[name] = next(f for f in env.files.values() if getattr(f, 'name', None) == name)
            hashes[name] = hashlib.sha256((source/name).read_bytes()).hexdigest()
        return files[name]

    def resolve(owner, ref):
        name = owner.name if not ref['m_FileID'] else Path(owner.externals[ref['m_FileID']-1].path).name
        return load(name).objects[ref['m_PathID']]

    owners = dict(COMPONENT_OWNERS)
    for folder, stem, level in [('sly-shop', 'sly-shop-actor', 17),
                                ('route-tolls', 'city-toll-actor', 80),
                                ('route-tolls', 'mines-toll-actor', 272),
                                ('greenpath-tolls', 'greenpath-toll-actor', 158)]:
        metadata = read_json(Path(root)/'_private/converted'/folder/(stem+'-source.json'))
        owners[f'{folder}/{stem}'] = (f'level{level}', metadata['owner'])
    output.mkdir(parents=True, exist_ok=True)
    for kind, (collection, identity) in owners.items():
        scene = load(collection)
        owner = scene.objects[identity].read_typetree()
        components = []
        for item in owner.get('m_Component', owner.get('m_Components', [])):
            obj = resolve(scene, item.get('component', item.get('m_Component')))
            data = obj.read_typetree()
            component = dict(collection=obj.assets_file.name, path_id=obj.path_id,
                             native_type=obj.type.name, data=data)
            body = structure(data)
            if body.get('m_Script', {}).get('m_PathID'):
                component['script_class'] = resolve(obj.assets_file, body['m_Script']).read_typetree()['m_ClassName']
            if 'fsm' in body and body['fsm'].get('states'):
                component['decoded'] = decode_fsm(body['fsm'])
            components.append(component)
        ancestors = []
        transform = next((c['data'] for c in components if c['native_type'] == 'Transform'), {})
        parent = transform.get('m_Father', {})
        while parent.get('m_PathID'):
            obj = resolve(scene, parent)
            data = obj.read_typetree()
            ancestors.append(dict(collection=obj.assets_file.name, path_id=obj.path_id, data=data))
            scene, parent = obj.assets_file, data.get('m_Father', {})
            if len(ancestors) > 32:
                raise ValueError(f'{kind}: cyclic/excessive transform ancestry')
        snapshot = dict(kind=kind, complete_owner=True, ancestors=ancestors, source_sha256=dict(hashes),
                        actors=[dict(collection=collection, path_id=identity,
                                     name=owner['m_Name'], components=components)])
        (output/(kind.replace('/', '__')+'.json')).write_text(json.dumps(snapshot, indent=2)+'\n', encoding='utf-8')


def export_special_libraries(root, output):
    """Repair selected-only shop/toll snapshots from their authored animators.

    Uses the same UnityPy reader as convert_sly_actor, without textures or writes
    to the source repository. Python must match the installed native packages.
    """
    sys.path.insert(0, str(Path(root)/'_private/tools/boss-python'))
    import UnityPy
    from watcher_unity import AlignedGenerator
    source = Path(root)/'_private/original/Hollow Knight/hollow_knight_Data'
    env = UnityPy.Environment(path=str(source))
    generator = AlignedGenerator('6000.0.61f1')
    generator.load_local_dll_folder(str(source/'Managed'))
    env.typetree_generator = generator
    files = {}

    def load(name):
        if name not in files:
            env.load_file(str(source/name))
            files[name] = next(f for f in env.files.values() if getattr(f, 'name', None) == name)
        return files[name]

    def resolve(owner, ref):
        name = owner.name if not ref['m_FileID'] else Path(owner.externals[ref['m_FileID']-1].path).name
        return load(name).objects[ref['m_PathID']]

    output.mkdir(parents=True, exist_ok=True)
    for folder, stem, level in [('sly-shop', 'sly-shop-actor', 17),
                                ('route-tolls', 'city-toll-actor', 80),
                                ('route-tolls', 'mines-toll-actor', 272),
                                ('greenpath-tolls', 'greenpath-toll-actor', 158)]:
        metadata = read_json(Path(root)/'_private/converted'/folder/(stem+'-source.json'))
        scene = load(f'level{level}')
        owner = scene.objects[metadata['owner']].read_typetree()
        animator = scene.objects[metadata['animator']].read_typetree()
        library = resolve(scene, animator['library'])
        clips = library.read_typetree()['clips']
        if not set(metadata['clips']) <= {c['name'] for c in clips}:
            raise ValueError(f'{stem}: selected clips absent from original library')
        snapshot = dict(scene=scene.name, owner=metadata['owner'], owner_name=owner['m_Name'],
                        animator=metadata['animator'], animator_data=animator,
                        library=f'{library.assets_file.name}:{library.path_id}', clips=clips,
                        source_sha256={name: hashlib.sha256((source/name).read_bytes()).hexdigest()
                                       for name in (scene.name, library.assets_file.name)})
        (output/(stem+'.json')).write_text(json.dumps(snapshot, indent=2)+'\n', encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=source_root())
    parser.add_argument('--output', type=Path, default=ROOT/'_private/converted-psp/actor-facing/config.json')
    parser.add_argument('--extract-libraries', action='store_true', help='repair shop/toll library snapshots using UnityPy')
    parser.add_argument('--extract-components', action='store_true', help='extend incomplete actor owners from the read-only original')
    parser.add_argument('--header', type=Path, help='write complete converted runtime config as a C++ header')
    args = parser.parse_args()
    if args.extract_libraries:
        export_special_libraries(args.source_root, args.output.parent/'source-libraries')
    if args.extract_components:
        export_original_components(args.source_root, args.output.parent/'source-components')
    from animation_audit import inventory
    actors = inventory(args.source_root, args.output.parent)
    result = export(args.source_root, actors, args.output)
    missing = sum(r['verdict'] != 'ok' for r in result.values())
    if args.header and missing and os.environ.get('HKPSP_FAST_CHECKPOINT') == '1':
        # Player builds ship the prebuilt EBOOT, which already contains this header; the PSP
        # data steps read only config.json (instance scales). Keep the tracked header.
        unresolved = sorted(k for k, r in result.items() if r['verdict'] != 'ok')
        print(f'facing export: {len(result)} actors; header kept, unresolved: {", ".join(unresolved)}')
        return 0
    if args.header:
        write_header(result, args.header)
    print(f'facing export: {len(result)} actors, {missing} unresolved')
    return int(missing != 0)


if __name__ == '__main__':
    raise SystemExit(main())
