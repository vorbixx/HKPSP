"""Compile authored player-triggered room masks, including inverse child masks."""
import struct
from convert_terrain import paths, transform_point
from inspect_fsm import decode_fsm
from scene_walls import key

# Audited per-item contracts. Other masks retain HKM1 until their source audit.
SOURCE_ONCE_FSMS = {
    # (clip, audio-player) fileIDs resolve to sharedassets6.assets-157 and
    # resources.assets-4127 in each original scene's dependency table.
    ('Crossroads_01', 'level37-4778.json'): (2, 5),
    ('Crossroads_04', 'level40-5105.json'): (6, 4),
    ('Crossroads_ShamanTemple', 'level76-14167.json'): (2, 6),
    ('Crossroads_04', 'level40-5042.json'): (6, 4),
    ('Crossroads_21', 'level58-5112.json'): (2, 9),
    ('Crossroads_21', 'level58-5162.json'): (2, 9),
    ('Fungus1_01', 'level128-7568.json'): (2, 7),
    ('Fungus1_06', 'level135-6445.json'): (2, 4),
    ('Fungus1_11', 'level140-6466.json'): (5, 4),
    ('Fungus2_10', 'level174-7580.json'): (2, 4),
    ('Fungus2_21', 'level186-5531.json'): (2, 4),
    ('Mines_33', 'level272-3049.json'): (2, 5),
}

def once_profile(fsm, name, sound_files=(2, 5)):
    """Recognize the persistent unmasker graph, including its discovery action."""
    states = {s['name']: s for s in fsm['states']}
    booleans = {v['name']: v['value'] for v in fsm['variables'].get('boolVariables', ())}
    if 'Activated' not in booleans:
        return None
    def require(condition):
        if not condition:
            raise ValueError('Unsupported persistent room-mask graph: ' + name)
    def actions(state, kinds):
        result = [a for a in states[state]['actions'] if a['enabled']]
        require([a['type'].rsplit('.', 1)[-1] for a in result] == kinds)
        return [a['parameters'] for a in result]
    def edge(state, event, destination):
        require([t['toState'] for t in states[state]['transitions'] if t['fsmEvent']['name'] == event] == [destination])
    require(set(states) == {'Pause', 'Idle', 'Idle Stay', 'Fade', 'Sound', 'Activate'})
    require(fsm['start'] == 'Pause' and booleans.get('Activated') is False)
    require(not fsm.get('globalTransitions'))
    wait, = actions('Pause', ['WaitForHeroInPosition'])
    require(wait['sendEvent'] == 'FINISHED')
    edge('Pause', 'FINISHED', 'Idle')
    test, = actions('Idle', ['BoolTest'])
    require(test['boolVariable'].get('variable') == 'Activated' and test['isTrue'] == 'ACTIVATE'
            and not test['isFalse'] and not test['everyFrame'])
    edge('Idle', 'ACTIVATE', 'Activate')
    edge('Idle', 'FINISHED', 'Idle Stay')
    trigger, = actions('Idle Stay', ['Trigger2dEvent'])
    require(trigger['trigger'] == 1 and trigger['collideTag']['value'] == 'Player')
    edge('Idle Stay', trigger['sendEvent'], 'Fade')
    fade, activate, sound_test = actions('Fade', ['iTweenFadeTo', 'SetBoolValue', 'BoolTest'])
    require(activate['boolVariable'].get('variable') == 'Activated'
            and activate['boolValue'].get('value') is True and not activate['everyFrame'])
    require(sound_test['boolVariable'].get('variable') == 'Play Sound' and sound_test['isTrue'] == 'SOUND'
            and not sound_test['isFalse'] and not sound_test['everyFrame'])
    edge('Fade', 'SOUND', 'Sound')
    reload, = actions('Activate', ['iTweenFadeTo'])
    for tween in (fade, reload):
        require(tween['gameObject']['ownerOption'] == 0 and tween['alpha'].get('value') == 0
                and not tween.get('startEvent') and not tween.get('finishEvent'))
    sound, = actions('Sound', ['AudioPlayerOneShotSingle'])
    require(sound['audioClip']['value'] == {'m_FileID': sound_files[0], 'm_PathID': 157}
            and sound['audioPlayer']['value'] == {'m_FileID': sound_files[1], 'm_PathID': 4127}
            and not sound['audioClip'].get('useVariable') and not sound['audioPlayer'].get('useVariable')
            and sound['spawnPoint'].get('useVariable') and sound['spawnPoint'].get('name') == 'Hero')
    require(all(sound[p].get('value') == value for p, value in
                (('pitchMin', 1), ('pitchMax', 1), ('volume', 1), ('delay', 0))))
    require(isinstance(booleans.get('Play Sound'), bool))
    require(not states['Sound']['transitions'] and not states['Activate']['transitions'])
    return booleans['Play Sound']

def compile_masks(mapping, read, visuals, visual_read):
    result, seen = [], set()
    for row in mapping['inventory']:
        group = row['components'][0]
        for name in group.get('PlayMakerFSM', ()):
            if name in seen:
                continue
            seen.add(name)
            component = read(name)
            raw = component['m_Structure'].get('fsm', {})
            if not component.get('m_Enabled', 1) or raw.get('name') not in ('unmasker', 'remasker_inverse'):
                continue
            if not all(read(k)['m_IsActive'] for k in row['owners']):
                continue
            root = row['go']
            children = [v for v in visuals['inventory'] if root in v['owners']]
            if not children:
                continue
            fsm = decode_fsm(raw)
            states = {s['name']: s for s in fsm['states']}
            variables = {v['name']: v['value'] for v in fsm['variables']['floatVariables']}
            actions = lambda state: [a for a in states.get(state, {}).get('actions', ()) if a['enabled']]
            idle = 'Idle Stay' if 'Idle Stay' in states else 'Idle'
            trigger = next((a['parameters'] for a in actions(idle) if a['type'].endswith('.Trigger2dEvent')), None)
            if trigger is None or trigger['collideTag']['value'] != 'Player' or trigger['trigger'] not in (0, 1):
                raise ValueError('Unsupported room-mask entry: ' + name)
            inside = next(t['toState'] for t in states[idle]['transitions'] if t['fsmEvent']['name'] == trigger['sendEvent'])
            exit_trigger = next((a['parameters'] for a in actions(inside) if a['type'].endswith('.Trigger2dEvent') and a['parameters']['trigger'] == 2), None)
            once = exit_trigger is None
            source_profile = (mapping['scene'], name) in SOURCE_ONCE_FSMS
            sound = once_profile(fsm, name, SOURCE_ONCE_FSMS[mapping['scene'], name]) if source_profile else None
            if source_profile and (not once or sound is None):
                raise ValueError('Changed source room-mask contract: ' + name)
            outside = None if once else next(t['toState'] for t in states[inside]['transitions'] if t['fsmEvent']['name'] == exit_trigger['sendEvent'])
            targets = {'': root}
            for action in actions(fsm['start']):
                if action['type'].endswith('.FindChild'):
                    params = action['parameters']
                    matches = {owner for visual in children for owner in visual['owners']
                               if owner != root and visual_read(owner)['m_Name'] == params['childName']['value']}
                    if len(matches) > 1:
                        raise ValueError('Ambiguous room-mask child: ' + name)
                    if matches:
                        targets[params['storeResult']['name']] = next(iter(matches))
            def fades(state):
                values = {}
                for action in actions(state):
                    if not action['type'].endswith('.iTweenFadeTo'):
                        continue
                    params = action['parameters']
                    target = '' if params['gameObject']['ownerOption'] == 0 else params['gameObject']['gameObject']['name']
                    if target not in targets:
                        continue
                    if (not params['includeChildren']['value'] or params['easeType'] != 21 or params['loopType'] != 0
                            or params['delay']['value'] != 0 or params['realTime']['value']):
                        raise ValueError('Unsupported room-mask fade: ' + name)
                    time = params['time']
                    seconds = variables[time['variable']] if 'variable' in time else time['value']
                    values[target] = (params['alpha']['value'], seconds)
                return values
            entering, leaving = fades(inside), fades(outside)
            idle_fades, initial = fades('Idle'), fades(fsm['start'])
            pause = max((a['parameters']['time']['value'] for a in actions(fsm['start']) if a['type'].endswith('.Wait')), default=0.)
            polygons = []
            for collider_row in mapping['inventory']:
                if collider_row['go'] != root:
                    continue
                collider = read(collider_row['collider'])
                if not collider.get('m_Enabled') or not collider.get('m_IsTrigger'):
                    continue
                chain = [read(k) for k in collider_row['chain']]
                polygons.extend([[transform_point(point, chain)[:2] for point in polygon]
                                 for polygon in paths(collider, collider_row['type'])])
            if not polygons:
                raise ValueError('Missing room-mask trigger: ' + name)
            persistent = None
            if once:
                if source_profile:
                    identities = [read(source) for source in group.get('PersistentBoolItem', ())]
                    if len(identities) != 1:
                        raise ValueError('Unsupported persistent room-mask identity: ' + name)
                    identity = identities[0]
                    data = identity['m_Structure']
                    if not identity.get('m_Enabled', 1) or data['dontSave'] or data['semiPersistent']:
                        raise ValueError('Unsupported persistent room-mask identity: ' + name)
                for source in group.get('PersistentBoolItem', ()):
                    data = read(source)['m_Structure']
                    if not data['dontSave'] and not data['semiPersistent']:
                        data = data['persistentBoolData']
                        persistent = (key(data['sceneName'] or mapping['scene']), key(data['id'] or read(root)['m_Name']))
            if sound is not None and (not persistent or pause):
                raise ValueError('Missing persistent room-mask identity or unsupported startup wait: ' + name)
            for target, (alpha, seconds) in entering.items():
                outside_alpha, leave_seconds = leaving.get(target, idle_fades.get(target, (1., seconds)))
                owners = []
                for visual in children:
                    controlled = [t for t in entering if targets[t] in visual['owners']]
                    if controlled and controlled[-1] == target:
                        owners.append(int(visual['go'].removesuffix('.json').rsplit('-', 1)[1]))
                if owners:
                    result.append(dict(owners=sorted(set(owners)), polygons=polygons, once=once, pause=pause,
                                       seconds=seconds, leave_seconds=leave_seconds,
                                       reload_seconds=fades('Activate').get(target, (alpha, seconds))[1],
                                       inside=alpha, outside=outside_alpha, initial=initial.get(target, (1., 0.))[0],
                                       persistent=persistent, fsm=name, source_tween=sound is not None,
                                       discovery_sound=bool(sound)))
    return result

def write_masks(path, masks):
    extended = any(m.get('source_tween') for m in masks)
    binary = bytearray(struct.pack('<4sI', b'HKM2' if extended else b'HKM1', len(masks)))
    for mask in masks:
        flags = int(mask['once']) | (2 if mask.get('source_tween') else 0) | (4 if mask.get('discovery_sound') else 0)
        binary += struct.pack('<3I7f2I', flags, *(mask['persistent'] or (0, 0)),
                              mask['pause'], mask['seconds'], mask['leave_seconds'], mask['reload_seconds'],
                              mask['inside'], mask['outside'], mask['initial'], len(mask['owners']), len(mask['polygons']))
        binary += struct.pack('<' + 'I' * len(mask['owners']), *mask['owners'])
        for polygon in mask['polygons']:
            binary += struct.pack('<I', len(polygon))
            for point in polygon:
                binary += struct.pack('<2f', *point)
    path.write_bytes(binary)
