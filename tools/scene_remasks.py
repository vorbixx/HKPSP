"""Compile reversible source covers, inverse masks and tablet masks together."""
import warnings
import json
import math
import struct
from pathlib import Path

from convert_terrain import paths, transform_point
from inspect_fsm import decode_fsm


def inverse_mask(source, row, fsm):
    """Inspect one saved inverse mask without changing source exports."""
    return _inverse_mask(lambda name: json.loads((source/name).read_text()), row, fsm)


def _inverse_mask(read, row, fsm):
    states = {state['name']: state for state in fsm['states']}
    if fsm['start'] != 'Pause' or read(row['chain'][0])['m_Children']:
        raise ValueError('requires childless Pause/Idle inverse mask')
    fades = {}
    for state, alpha, event, trigger in [('Idle', 0, 'UNCOVER', 1),
                                        ('Fade Out', 1, 'COVER', 2),
                                        ('Fade In', 0, 'UNCOVER', 1)]:
        actions = [a for a in states[state]['actions'] if a['enabled']]
        fade = next(a['parameters'] for a in actions if a['type'].endswith('.iTweenFadeTo')
                    and a['parameters']['gameObject']['ownerOption'] == 0)
        contact = next(a['parameters'] for a in actions if a['type'].endswith('.Trigger2dEvent'))
        if (fade['alpha']['value'] != alpha or not fade['includeChildren']['value'] or
                fade['easeType'] != 21 or fade['loopType'] != 0 or fade['delay']['value'] != 0 or
                not fade['stopOnExit']['value'] or contact['trigger'] != trigger or
                contact['collideTag']['value'] != 'Player' or contact['sendEvent'] != event):
            raise ValueError('unsupported inverse mask fade/contact')
        fades[state] = fade
    seconds = next(v['value'] for v in fsm['variables']['floatVariables'] if v['name'] == 'Fade Time')
    if any(fades[state]['time'].get('variable') != 'Fade Time' for state in ('Fade Out', 'Fade In')):
        raise ValueError('unsupported inverse mask fade duration')
    pause = next(a['parameters']['time']['value'] for a in states['Pause']['actions']
                 if a['enabled'] and a['type'].endswith('.Wait'))
    startup = fades['Idle']['time']['value']
    if not all(math.isfinite(v) and v >= 0 for v in (seconds, pause, startup)):
        raise ValueError('invalid inverse mask timing')
    collection = row['go'].rsplit('-', 1)[0]
    components = [f"{collection}-{c['m_Component']['m_PathID']}.json"
                  for c in read(row['go'])['m_Components']]
    colliders = []
    for name in components:
        try:
            colliders.append(read(name))
        except FileNotFoundError:
            continue
    colliders = [c for c in colliders if c.get('m_Enabled') and c.get('m_IsTrigger')]
    if len(colliders) != 1:
        raise ValueError('requires one inverse mask trigger')
    collider = colliders[0]
    kind = 'PolygonCollider2D' if 'm_Points' in collider else 'BoxCollider2D'
    polygons = paths(collider, kind)
    if len(polygons) != 1:
        raise ValueError('requires one inverse mask polygon')
    chain = [read(name) for name in row['chain']]
    points = [transform_point(p, chain) for p in polygons[0]]
    bounds = [min(p[0] for p in points), min(p[1] for p in points),
              max(p[0] for p in points), max(p[1] for p in points)]
    return dict(owner=int(row['go'].rsplit('-', 1)[1].split('.')[0]), bounds=bounds,
                polygon=points if kind == 'PolygonCollider2D' else [],
                seconds=seconds, pause=pause, startup=startup)


def _id(name):
    return int(Path(name).stem.rsplit('-', 1)[1])


def _sprite_owners(mapping, root):
    return sorted({_id(row['go']) for row in mapping['inventory']
                   if row.get('active') and row.get('components', {}).get('SpriteRenderer')
                   and (row['go'] == root or root in row.get('owners', []))})


def _rect(points):
    return [min(p[0] for p in points), min(p[1] for p in points),
            max(p[0] for p in points), max(p[1] for p in points)]


def _profile(fsm):
    """Recognize the source stay/exit fade graph; never infer progression gates."""
    states = {s['name']: s for s in fsm['states']}
    expected = {'Pause': {'FINISHED': 'Idle'}, 'Idle': {'UNCOVER': 'Fade Out'},
                'Fade Out': {'COVER': 'Fade In'}, 'Fade In': {'UNCOVER': 'Fade Out'}}
    globals_ = {t['fsmEvent']['name']: t['toState'] for t in fsm.get('globalTransitions', [])}
    if fsm.get('start') != 'Pause' or globals_ not in ({}, {'HERO LEAVE': 'Hero Leave'}):
        return None
    actions = {}
    allowed = {'Trigger2dEvent', 'iTweenFadeTo', 'Wait', 'WaitForHeroInPosition', 'SetBoolValue'}
    for name, state in states.items():
        transitions = {t['fsmEvent']['name']: t['toState'] for t in state.get('transitions', [])}
        if transitions != expected.get(name, {}):
            return None
        actions[name] = {}
        for action in state.get('actions', []):
            if not action.get('enabled'):
                continue
            kind = action['type'].rsplit('.', 1)[-1]
            if kind not in allowed or kind in actions[name]:
                return None
            params = action['parameters']
            if kind == 'SetBoolValue' and params['boolVariable'] != {'variable': 'Activated'}:
                return None
            actions[name][kind] = params
    if not expected.keys() <= states.keys():
        return None
    variables = {v['name']: v['value'] for v in fsm['variables']['floatVariables']}

    def scalar(value):
        result = variables[value['variable']] if 'variable' in value else value['value']
        if not isinstance(result, (int, float)) or not math.isfinite(result):
            raise ValueError('unmasker needs a finite source constant')
        return result

    try:
        duration = None
        for name, alpha, trigger, event in [('Fade Out', 0, 2, 'COVER'),
                                            ('Fade In', 1, 1, 'UNCOVER'),
                                            ('Idle', None, 1, 'UNCOVER')]:
            params = actions[name]['Trigger2dEvent']
            if (params['trigger'] != trigger or params['sendEvent'] != event or
                    params['collideTag'].get('value') != 'Player'):
                return None
            if alpha is None:
                continue
            params = actions[name]['iTweenFadeTo']
            seconds = scalar(params['time'])
            if (scalar(params['alpha']) != alpha or not scalar(params['includeChildren']) or
                    params['easeType'] != 21 or params['loopType'] != 0 or
                    scalar(params['delay']) != 0 or scalar(params['realTime']) or
                    not scalar(params['stopOnExit']) or params['gameObject']['ownerOption'] != 0 or
                    params['namedValueColor'].get('value') != '_Color' or
                    not 0 < seconds <= 10 or (duration is not None and seconds != duration)):
                return None
            duration = seconds
        wait = actions['Pause']['Wait']
        positioned = actions['Pause']['WaitForHeroInPosition']
        pause = scalar(wait['time'])
        if (wait['finishEvent'] != 'FINISHED' or wait['realTime'] or
                positioned['sendEvent'] != 'FINISHED' or
                scalar(positioned['skipIfAlreadyPositioned']) or not 0 < pause <= 10):
            return None
    except (KeyError, TypeError, ValueError):
        return None
    return dict(seconds=duration, opaque_inside=False, startup_pause=pause,
                startup_fade=0., startup_hero_positioned=True)


def _tablet_profile(control, fade, child_name):
    """Recognize charm-gated proximity glyphs, retaining their startup predicate."""
    def actions(fsm, graph, kinds):
        if fsm.get('globalTransitions') or fsm.get('manualUpdate'):
            raise ValueError('external fade transition')
        states = {s['name']: s for s in fsm['states']}
        if set(states) != set(graph):
            raise ValueError('unknown fade state')
        result = {}
        for name, state in states.items():
            edges = {t['fsmEvent']['name']: t['toState'] for t in state.get('transitions', [])}
            enabled = [a for a in state['actions'] if a['enabled']]
            result[name] = {a['type'].rsplit('.', 1)[-1]: a['parameters'] for a in enabled}
            if (edges != graph[name] or len(enabled) != len(kinds[name]) or
                    sorted(a['type'].rsplit('.', 1)[-1] for a in enabled) != sorted(kinds[name])):
                raise ValueError('unknown fade action')
        return result
    def scalar(p, name):
        field = p[name]
        if field.get('useVariable') is not False:
            raise ValueError('variable fade parameter')
        return field['value']
    try:
        if control['start'] != 'Pause' or fade['start'] != 'Idle':
            return None
        c = actions(control, {
            'Pause': {'FINISHED': 'Init'}, 'Init': {'FINISHED': 'Out', 'INACTIVE': 'Inactive'},
            'Out': {'ENTER': 'Play Particle'}, 'Play Particle': {'FINISHED': 'Enter'},
            'Enter': {'EXIT': 'Exit'}, 'Exit': {'ENTER': 'Enter'}, 'Inactive': {}}, {
            'Pause': ['NextFrameEvent'], 'Init': ['PlayerDataBoolTest', 'FindChild', 'FindChild'],
            'Out': ['Trigger2dEvent'], 'Play Particle': ['PlayParticleEmitter'],
            'Enter': ['Trigger2dEvent', 'SetParticleEmission', 'SendEventByName'],
            'Exit': ['Trigger2dEvent', 'SetParticleEmission', 'SendEventByName'], 'Inactive': []})
        predicate = c['Init']['PlayerDataBoolTest']
        key = scalar(predicate, 'boolName')
        if (not key.startswith('equippedCharm_') or predicate['isTrue'] or
                predicate['isFalse'] != 'INACTIVE' or c['Pause']['NextFrameEvent']['sendEvent'] != 'FINISHED'):
            return None
        charm = int(key.removeprefix('equippedCharm_'))
        if not 1 <= charm <= 32:
            return None
        find = [a['parameters'] for s in control['states'] if s['name'] == 'Init'
                for a in s['actions'] if a['enabled'] and a['type'].endswith('.FindChild')]
        if not any(p['gameObject']['ownerOption'] == 0 and scalar(p, 'childName') == child_name and
                   p['storeResult'].get('name') == 'Fade Sprites' for p in find):
            return None
        for state, trigger, event in [('Out', 1, 'ENTER'), ('Enter', 2, 'EXIT'), ('Exit', 1, 'ENTER')]:
            p = c[state]['Trigger2dEvent']
            if p['trigger'] != trigger or p['sendEvent'] != event:
                return None
            # The authored Hero Detector collider has no tag/layer filter.
            for field, variable in [('collideTag', True), ('collideLayer', False)]:
                q = p[field]
                if q['useVariable'] is not variable or q['name'] or q['value']:
                    return None
            q = p['storeCollider']
            if (q['useVariable'] is not True or q['name'] or
                    q['value'] != {'m_FileID': 0, 'm_PathID': 0}):
                return None
        for state, event in [('Enter', 'FADE IN'), ('Exit', 'FADE OUT')]:
            p = c[state]['SendEventByName']
            target = p['eventTarget']
            if (scalar(p, 'sendEvent') != event or p['everyFrame'] or scalar(p, 'delay') != 0 or
                    target['target'] != 1 or target['gameObject']['ownerOption'] != 1 or
                    target['gameObject']['gameObject'].get('name') != 'Fade Sprites' or
                    scalar(target, 'sendToChildren') or scalar(target, 'fsmName')):
                return None
        f = actions(fade, {'Idle': {'FADE IN': 'Fade In'}, 'Fade In': {'FADE OUT': 'Fade Out'},
                           'Fade Out': {'FADE IN': 'Fade In'}},
                    {name: ['iTweenFadeTo'] for name in ('Idle', 'Fade In', 'Fade Out')})
        durations = []
        for state, alpha in [('Idle', 0), ('Fade In', 1), ('Fade Out', 0)]:
            p = f[state]['iTweenFadeTo']
            seconds = scalar(p, 'time')
            if (p['gameObject']['ownerOption'] != 0 or scalar(p, 'alpha') != alpha or
                    not scalar(p, 'includeChildren') or scalar(p, 'namedValueColor') != '_Color' or
                    scalar(p, 'delay') != 0 or p['easeType'] != 21 or p['loopType'] != 0 or
                    scalar(p, 'realTime') or not scalar(p, 'stopOnExit') or
                    p['startEvent'] or p['finishEvent'] or not 0 < seconds <= 10):
                return None
            durations.append(seconds)
        if durations[0] > .010001 or durations[1] != durations[2]:
            return None
        return dict(seconds=durations[1], opaque_inside=True, startup_pause=0., startup_fade=0.,
                    startup_hero_positioned=False, required_charm=charm)
    except (KeyError, TypeError, ValueError):
        return None


def _tablet_masks(mapping, read):
    rows = {r['go']: r for r in mapping['inventory']}
    result = []
    for child in mapping['inventory']:
        if not child.get('active') or len(child.get('owners', [])) < 2:
            continue
        parent = rows.get(child['owners'][1])
        if not parent or not parent.get('active'):
            continue
        for ref in child.get('components', {}).get('PlayMakerFSM', []):
            component = read(ref)
            if not component.get('m_Enabled', 1):
                continue
            raw = component.get('m_Structure', {}).get('fsm', {})
            if raw.get('name') != 'Fade':
                continue
            fade = decode_fsm(raw)
            for control_ref in parent.get('components', {}).get('PlayMakerFSM', []):
                component = read(control_ref)
                if not component.get('m_Enabled', 1):
                    continue
                raw = component.get('m_Structure', {}).get('fsm', {})
                if raw.get('name') != 'Control':
                    continue
                profile = _tablet_profile(decode_fsm(raw), fade, read(child['go'])['m_Name'])
                if profile is None or read(parent['go']).get('m_Layer') != 13:  # Hero Detector.
                    continue
                chain = [read(name) for name in parent['chain']]
                polygons = [[transform_point(p, chain) for path in paths(collider, 'BoxCollider2D') for p in path]
                            for name in parent['components'].get('BoxCollider2D', [])
                            for collider in [read(name)] if collider.get('m_Enabled') and collider.get('m_IsTrigger')]
                if not polygons or any(len(p) != 4 for p in polygons):
                    raise ValueError('Unsupported tablet fade trigger')
                owners = _sprite_owners(mapping, child['go'])
                if not owners:
                    continue
                result.append(dict(trigger=_rect([p for poly in polygons for p in poly]),
                                   polygon=polygons[0], polygons=polygons, owners=owners,
                                   source=ref, control_source=control_ref, **profile))
    return result


def observe(visual_source):
    """Return reversible source covers without dropping inverse polygons."""
    visual_source = Path(visual_source)
    mapping = json.loads((visual_source / 'room-visuals-map.json').read_text())
    read = lambda name: json.loads((visual_source / name).read_text())
    return collect_remasks(mapping, read)


def collect_remasks(mapping, read):
    result = []
    for row in mapping['inventory']:
        if not row.get('active'):
            continue
        for fsm_name in row.get('components', {}).get('PlayMakerFSM', []):
            fsm = decode_fsm(read(fsm_name)['m_Structure']['fsm'])
            if fsm.get('name') != 'unmasker':
                continue
            profile = _profile(fsm)
            colliders = row.get('components', {}).get('BoxCollider2D', [])
            if profile is None or not colliders:
                continue
            chain = [read(name) for name in row['chain']]
            polygons = []
            for name in colliders:
                collider = read(name)
                if not collider.get('m_Enabled') or not collider.get('m_IsTrigger'):
                    continue
                corners = [transform_point(point, chain)
                           for path in paths(collider, 'BoxCollider2D') for point in path]
                if len(corners) != 4:
                    raise ValueError(f'{fsm_name}: unmasker box must have four corners')
                polygons.append(corners)
            if not polygons:
                continue
            owners = _sprite_owners(mapping, row['go'])
            if owners:
                result.append(dict(trigger=_rect([p for poly in polygons for p in poly]),
                                   polygon=polygons[0], polygons=polygons, owners=owners,
                                   source=fsm_name, **profile))
    for mask in compile_remasks(mapping, read, profiles=('remasker_inverse', 'remasker')):
        result.append(dict(trigger=mask['trigger'], owners=mask['owners'], seconds=mask['seconds'],
                           polygon=mask['polygon'], polygons=[mask['polygon']] if mask['polygon'] else [],
                           opaque_inside=mask['opaque_inside'],
                           startup_pause=mask['pause'], startup_fade=mask['startup'],
                           startup_hero_positioned=mask['startup_hero_positioned']))
    return result + _tablet_masks(mapping, read)


def write(remasks, path):
    if len(remasks) > 16:
        raise ValueError('scene exceeds runtime remask capacity')
    gated = any(r.get('required_charm', 0) for r in remasks)
    flexible = any(len(p) != 4 for r in remasks
                   for p in r.get('polygons', [r['polygon']] if r.get('polygon') else []))
    multipart = gated or any(len(r.get('polygons', [])) > 1 for r in remasks)
    multipart = multipart or flexible
    data = bytearray(struct.pack('<4sI', b'HKS5' if flexible else b'HKS4' if gated else b'HKS3' if multipart else b'HKS2', len(remasks)))
    for remask in remasks:
        owners = remask['owners']
        polygon = remask.get('polygon', [])
        polygons = remask.get('polygons', [polygon] if polygon else [])
        if not 0 < len(owners) <= 128 or owners != sorted(set(owners)) or min(owners) <= 0:
            raise ValueError('remask owners must be positive, unique and sorted')
        if len(polygons) > 16 or any(not 3 <= len(p) <= 128 for p in polygons) or sum(map(len, polygons)) > 128:
            raise ValueError('remask polygon exceeds runtime contour/point capacity')
        data += struct.pack('<4fI', *remask['trigger'], len(owners))
        data += struct.pack('<%dI' % len(owners), *owners)
        flags = (1 if remask['opaque_inside'] else 0) | (2 if remask['startup_hero_positioned'] else 0)
        data += struct.pack('<fIffI', remask['seconds'], flags,
                            remask['startup_pause'], remask['startup_fade'],
                            len(polygons) if multipart else (4 if polygons else 0))
        for points in polygons:
            if flexible:
                data += struct.pack('<I', len(points))
            for point in points:
                data += struct.pack('<2f', *point)
        if gated or flexible:
            charm = remask.get('required_charm', 0)
            if not isinstance(charm, int) or not 0 <= charm <= 32:
                raise ValueError('Invalid remask charm predicate')
            data += struct.pack('<I', charm)
    Path(path).write_bytes(data)

def compile_remasks(mapping, read, profiles=None):
    masks = []
    seen = set()
    inventory = mapping['inventory']
    for row in inventory:
        if not row.get('active', True):
            continue
        for key in row.get('components', {}).get('PlayMakerFSM', []):
            if key in seen:
                continue
            seen.add(key)
            component = read(key)
            if not component.get('m_Enabled', 1):
                continue
            raw = component.get('m_Structure', {}).get('fsm') or {}
            if profiles is not None and raw.get('name') not in profiles:
                continue
            if raw.get('name') == 'remasker_inverse':
                try:
                    mask = _inverse_mask(read, row, decode_fsm(raw))
                except (ValueError, KeyError, StopIteration) as error:
                    warnings.warn(f'{mapping.get("scene", "room")}: inverse mask {key} omitted: {error}')
                    continue
                masks.append(dict(trigger=mask['bounds'], seconds=mask['seconds'],
                                  pause=mask['pause'], startup=mask['startup'],
                                  owners=[mask['owner']], polygon=mask['polygon'],
                                  # The authored inverse FSM fades to alpha 0
                                  # in Idle, then uses alpha 1 on UNCOVER.
                                  opaque_inside=True, startup_hero_positioned=True))
                continue
            names = {state['name'] for state in raw.get('states', [])}
            if raw.get('name') not in ('unmasker', 'remasker') or not {'Idle', 'Fade Out', 'Fade In'} <= names:
                continue
            fsm = decode_fsm(raw)
            states = {state['name']: state for state in fsm['states']}
            if fsm['start'] not in ('Idle', 'Pause'):
                raise ValueError('Unsupported remask startup')
            seconds = next(v['value'] for v in fsm['variables']['floatVariables'] if v['name'] == 'Fade Time')
            if not 0 < seconds <= 60:
                raise ValueError('Invalid remask fade duration')
            for name, event, trigger, destination, alpha in (
                    ('Idle', 'UNCOVER', 1, 'Fade Out', None),
                    ('Fade Out', 'COVER', 2, 'Fade In', 0),
                    ('Fade In', 'UNCOVER', 1, 'Fade Out', 1)):
                state = states[name]
                if not any(t['fsmEvent']['name'] == event and t['toState'] == destination for t in state['transitions']):
                    raise ValueError('Unsupported remask transition')
                actions = [a for a in state['actions'] if a['enabled']]
                p = next(a['parameters'] for a in actions if a['type'].endswith('.Trigger2dEvent'))
                if p['trigger'] != trigger or p['collideTag']['value'] != 'Player' or p['sendEvent'] != event:
                    raise ValueError('Unsupported remask trigger')
                if alpha is not None:
                    p = next(a['parameters'] for a in actions if a['type'].endswith('.iTweenFadeTo'))
                    if (p['gameObject']['ownerOption'] != 0 or p['alpha']['value'] != alpha or
                            not p['includeChildren']['value'] or p['time'].get('variable') != 'Fade Time' or
                            p['easeType'] != 21 or p['loopType'] != 0 or p['delay']['value'] != 0 or
                            p['realTime']['value'] or not p['stopOnExit']['value']):
                        raise ValueError('Unsupported remask tween')
            pause = 0
            if fsm['start'] == 'Pause':
                waits = [a['parameters'] for a in states['Pause']['actions']
                         if a['enabled'] and a['type'].endswith('.Wait')]
                if len(waits) != 1 or waits[0]['realTime'] or not 0 <= waits[0]['time']['value'] <= 60:
                    raise ValueError('Unsupported remask pause')
                pause = waits[0]['time']['value']
            chain = [read(key) for key in row['chain']]
            polygons = []
            for kind in ('BoxCollider2D', 'PolygonCollider2D'):
                for key in row['components'].get(kind, []):
                    collider = read(key)
                    if collider.get('m_Enabled') and collider.get('m_IsTrigger'):
                        polygons.extend([[transform_point(p, chain) for p in polygon] for polygon in paths(collider, kind)])
            # Prefab FSM copies without a trigger cannot receive local stay/exit
            # callbacks; keep their serialized visibility, as for other receivers.
            if not polygons:
                continue
            if len(polygons) != 1 or not 3 <= len(polygons[0]) <= 128:
                raise ValueError(f'Unsupported remask trigger geometry: {row["go"]} ({len(polygons)} paths)')
            points = polygons[0]
            owners = sorted({int(r['go'].rsplit('-', 1)[1].split('.')[0])
                             for r in inventory if row['go'] in r['owners']})
            if not 1 <= len(owners) <= 128:
                raise ValueError('Invalid remask owner count')
            masks.append(dict(trigger=[min(p[0] for p in points), min(p[1] for p in points),
                                       max(p[0] for p in points), max(p[1] for p in points)],
                              seconds=seconds, pause=pause, startup=0, owners=owners, polygon=points,
                              opaque_inside=False, startup_hero_positioned=False))
    if len(masks) > 16:
        raise ValueError('Room exceeds the existing remask fade pool')
    return masks


def export(mapping, read, target):
    masks = collect_remasks(mapping, read)
    write(masks, target / 'scene-remasks.bin')
    return masks
