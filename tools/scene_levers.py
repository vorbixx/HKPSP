"""Reviewed Switch Control/Mantis Gate pairs, never unconditional scenery."""
import hashlib
import json
import math
import struct
from inspect_fsm import decode_fsm
from scene_walls import key, identity

# Semantic action/transition fingerprints of the inspected source templates.
# Source object references are rebound through each instance's Target variable.
SIGNATURES = {
    'Mantis Gate': '9973dda42716b4d9992195040910e0298229f512a30b0b94beb60f22514ded2c',
    'Mantis Lever': '2222f9e488fe1680277e9d6ebc59d06bebef329e6f3f4f81cc09e417456329cf',
    'Ruins Lever 1': '51541e3139f4469e21b52214f6370e44551a0b684d91ca91de60a99f1e3773b9',
    'Ruins Gate 1': '5ad4fa45c935ab825d1ec3872f7464219164a57e02a0ad16ed90c168d8f29376',
    'Range': '064c2228be77fe934364d90d956ae3037024f93a550668dd292ed1777b5096e7',
}

# Exact controller and clip admission for the recovered Ruins gate.
# Missing animation supplements leave the source gate solid.
SOURCE_PROFILES = {
    'Ruins1_05': dict(lever='Ruins Lever', gate='Ruins Gate 4',
                      lever_signature=SIGNATURES['Ruins Lever 1'],
                      gate_signature=SIGNATURES['Ruins Gate 1'],
                      target='Target 1', clips=('Gate Open',)),
    'Ruins1_05b': dict(lever='Ruins Lever 1', gate='Ruins Gate 1',
                       lever_signature=SIGNATURES['Ruins Lever 1'],
                       gate_signature=SIGNATURES['Ruins Gate 1'],
                       target='Target 1', clips=('Gate Open',)),
    'Ruins1_05c': dict(lever='Ruins Lever 3',
                       lever_signature='28db9cc2a1b224373b1d552cd5bd576d4ddcfe6a6c72dc91bbdae55a7892fe79',
                       gate_signature=SIGNATURES['Ruins Gate 1'],
                       targets=(('Target 1', 'Ruins Gate 3a'), ('Target 2', 'Ruins Gate 3b'),
                                ('Target 3', 'Ruins Gate 3c')), clips=('Gate Open',),
                       player_data='ruins1_5_tripleDoor'),
}
SIGNATURES.update({'Pillar Control': '3113e78be1395c2ed88194583bd0090282b8b2702dce5ae583de14dea50322f0',
                   'Pillar Switch': '840f1340895cff0728902e4c8cc876d74038b55a7076337bdf5961d33f3bca3f'})
SIGNATURES['Switch Control']=SIGNATURES['Mantis Lever']


def signature(fsm):
    def normalize(value):
        if isinstance(value, list):
            return [normalize(v) for v in value]
        if not isinstance(value, dict):
            return value
        if set(value) == {'m_FileID', 'm_PathID'}:
            return {'source_reference': bool(value['m_PathID'])}
        return {k: normalize(v) for k, v in value.items()
                if k not in {'tooltip', 'showInInspector', 'networkSync'}}
    def transitions(rows):
        return [(r['fsmEvent']['name'], r['toState']) for r in rows]
    value = dict(name=fsm['name'], start=fsm['start'],
                 global_transitions=transitions(fsm.get('globalTransitions', [])),
                 states=[dict(name=s['name'], transitions=transitions(s['transitions']),
                              actions=[{k: a[k] for k in ('type', 'enabled', 'parameters')}
                                       for a in s['actions']]) for s in fsm['states']])
    return hashlib.sha256(json.dumps(normalize(value), sort_keys=True).encode()).hexdigest()


def variable(fsm, kind, name):
    return next(v['value'] for v in fsm['variables'][kind] if v['name'] == name)


def controllers(mapping, read):
    profile = SOURCE_PROFILES.get(mapping.get('scene'))
    rows = {r['go']: r for r in mapping.get('mesh_renderer_audit', [])}
    result = []
    for row in rows.values():
        parts = row['components']
        pillar = row['name'] == 'Gate Switch'
        expected_name = profile['lever'] if profile else 'Mantis Lever'
        if row['name'] not in (expected_name, 'Gate Switch') or len(parts.get('PlayMakerFSM', [])) != 1 or len(parts.get('PersistentBoolItem', [])) != 1:
            continue
        fsm = decode_fsm(read(parts['PlayMakerFSM'][0])['m_Structure']['fsm'])
        expected_signature = profile['lever_signature'] if profile else SIGNATURES['Pillar Switch' if pillar else 'Mantis Lever']
        if signature(fsm) != expected_signature:
            continue
        if variable(fsm, 'boolVariables', 'Activated') or (not pillar and variable(fsm, 'boolVariables', 'In Range')):
            continue
        if not pillar:
            player_data = profile.get('player_data') if profile else None
            if variable(fsm, 'boolVariables', 'SetPlayerData') != bool(player_data):
                continue
            if player_data and variable(fsm, 'stringVariables', 'Player Data') != player_data:
                continue
        persistent = read(parts['PersistentBoolItem'][0])['m_Structure']
        if persistent['semiPersistent'] or persistent['dontSave'] or persistent['persistentBoolData']['activated']:
            continue
        targets = profile.get('targets') if profile else None
        if targets:
            # All three source OPEN/ACTIVATE recipients share this switch's
            # nail condition and persistence, but own independent colliders.
            linked = []
            for target_name, expected_gate in targets:
                linked.extend(controller_target(rows, row, parts, fsm, persistent, pillar,
                                                profile, read, target_name, expected_gate))
            if len(linked) == len(targets):
                result.extend(linked)
            continue
        target_name = profile['target'] if profile else 'Target'
        expected_gate = profile['gate'] if profile else ('Raising Pillar' if pillar else 'Mantis Gate')
        result.extend(controller_target(rows, row, parts, fsm, persistent, pillar,
                                        profile, read, target_name, expected_gate))
    return result


def controller_target(rows, row, parts, fsm, persistent, pillar, profile, read, target_name, expected_gate):
    target = variable(fsm, 'gameObjectVariables', target_name)
    if target['m_FileID'] != 0 or not target['m_PathID']:
        return []
    gate_name = row['go'].rsplit('-', 1)[0] + '-' + str(target['m_PathID']) + '.json'
    gate = rows.get(gate_name)
    if not gate or gate['name'] != expected_gate or len(gate['components'].get('PlayMakerFSM', [])) != 1:
        return []
    gate_fsm = decode_fsm(read(gate['components']['PlayMakerFSM'][0])['m_Structure']['fsm'])
    expected_gate_signature = profile['gate_signature'] if profile else SIGNATURES['Pillar Control' if pillar else 'Mantis Gate']
    if signature(gate_fsm) != expected_gate_signature:
        return []
    allowed = {'GameObject', 'Transform', 'MeshFilter', 'MeshRenderer', 'tk2dSprite', 'tk2dSpriteAnimator', 'PlayMakerFSM'}
    if set(parts) - allowed - {'PersistentBoolItem', 'PolygonCollider2D', 'BoxCollider2D'}:
        return []
    if set(gate['components']) - allowed - {'BoxCollider2D', 'TinkEffect', 'PersistentBoolItem', 'DeactivateIfPlayerdataTrue'}:
        return []
    return [dict(lever=row, gate=gate, persistent=persistent, fsm=fsm, gate_fsm=gate_fsm, pillar=pillar)]


def wait(fsm, state):
    actions = next(s['actions'] for s in fsm['states'] if s['name'] == state)
    p = next(a['parameters'] for a in actions if a['enabled'] and a['type'].endswith('.Wait'))
    assert not p['realTime'] and not p['time']['useVariable']
    return p['time']['value']


def observe_levers(mapping, read, visual_source, transform_point, require):
    # The arena's five gates are owned by mantis_switches and its native art.
    if mapping.get('scene') == 'Fungus2_15':
        return [], set()
    manifest = visual_source / 'tk2d/manifest.json'
    profile = SOURCE_PROFILES.get(mapping.get('scene'))
    # Most terrain fixtures and admitted scenes do not carry a visual
    # supplement. Preserve the established no-op path before trying to read
    # the optional visual map; a controller alone cannot supply clip timing.
    if not manifest.is_file():
        return [], set()
    visual_map_path = visual_source / 'room-visuals-map.json'
    if not visual_map_path.is_file():
        if profile is None:
            return [], set()
        raise ValueError('Missing source Ruins lever visual map')
    visual_map = json.loads(visual_map_path.read_bytes())
    pairs = controllers(visual_map, lambda n: json.loads((visual_source / n).read_bytes()))
    supplement = json.loads(manifest.read_bytes())
    if not supplement.get('levers'):
        return [], set()
    declared = {(a['owner'], a['gate']) for a in supplement['levers']}
    pairs = [p for p in pairs if (p['lever']['go'],p['gate']['go']) in declared]
    require(len(pairs) == len(declared), 'Changed source lever controller')
    require(supplement['room_manifest_sha256'] == hashlib.sha256((visual_source / 'room-visuals-map.json').read_bytes()).hexdigest(), 'Changed lever artwork anchors')
    result = []
    for pair in pairs:
        owner, gate = pair['lever']['go'], pair['gate']['go']
        art = next((a for a in supplement.get('levers', []) if a['owner'] == owner and a['gate'] == gate), None)
        require(art is not None, 'Re-extract the source lever/gate supplement')
        library = art['library']
        require(library.startswith('tk2d/') and '..' not in library.split('/'), 'Invalid lever library path')
        raw = (visual_source / library).read_bytes()
        require(hashlib.sha256(raw).hexdigest() == supplement['source_sha256'][library.removeprefix('tk2d/')], 'Changed lever animation library')
        clips = {c['name']: c for c in json.loads(raw)['m_Structure']['clips']}
        def duration(name):
            clip = clips[name]
            require(clip['fps'] > 0 and clip['wrapMode'] == 2 and clip['frames'], 'Unsupported gate animation')
            return len(clip['frames']) / clip['fps']
        if pair['pillar']:
            selected = [r for r in mapping['inventory'] if r['go'] == owner]
            require(len(selected) == 1 and selected[0]['type'] == 'BoxCollider2D', 'Unsupported pillar switch hit box')
            hit = selected[0]; collider = read(hit['collider'])
            require(collider['m_IsTrigger'] and collider['m_Enabled'], 'Disabled pillar switch hit box')
            offset, size = collider['m_Offset'], collider['m_Size']
            chain = [read(n) for n in hit['chain']]
            polygon = [transform_point([offset['m_X']+x*size['m_X']/2, offset['m_Y']+y*size['m_Y']/2], chain)
                       for x,y in ((-1,-1),(1,-1),(1,1),(-1,1))]
            bounds = [min(p[0] for p in polygon), min(p[1] for p in polygon), max(p[0] for p in polygon), max(p[1] for p in polygon)]
            gates = [r for r in mapping['inventory'] if r['go'] == gate and r['type'] == 'BoxCollider2D']
            require(len(gates) == 1, 'Missing pillar collider')
            gate_persistent = read(pair['gate']['components']['PersistentBoolItem'][0])['m_Structure']
            require(not gate_persistent['semiPersistent'] and not gate_persistent['dontSave'] and
                    not gate_persistent['persistentBoolData']['activated'], 'Unsupported pillar persistence')
            def identity_key(data, obj):
                data=data['persistentBoolData']
                return [key(data['sceneName'] or mapping['scene']), key(data['id'] or read(obj)['m_Name'])]
            require(variable(pair['fsm'], 'boolVariables', 'SetPlayerData') and
                    variable(pair['fsm'], 'stringVariables', 'Player Data') == 'shamanPillar', 'Changed pillar PlayerData binding')
            require(clips['Pillar Shake']['fps'] > 0 and clips['Pillar Shake']['wrapMode'] == 0 and
                    len(clips['Pillar Opened']['frames']) == 1, 'Unsupported pillar poses')
            result.append(dict(owner=identity(owner), gate=identity(gate), collider=identity(gates[0]['collider']),
                               key=identity_key(gate_persistent,gate), secondary_key=identity_key(pair['persistent'],owner),
                               player_data=key('shamanPillar'), pillar=True, range=bounds, polygon=polygon,
                               seconds=[wait(pair['fsm'],'Hit'),wait(pair['gate_fsm'],'Shake'),0,duration('Pillar Open'),wait(pair['gate_fsm'],'Finish Opening')],
                               shake_fps=clips['Pillar Shake']['fps']))
            continue
        selected = [r for r in mapping['inventory'] if r['go'] == owner]
        require(len(selected) == 1 and selected[0]['type'] == 'PolygonCollider2D', 'Unsupported lever hit polygon')
        hit = selected[0]
        collider = read(hit['collider'])
        require(collider['m_IsTrigger'] and collider['m_Enabled'] and len(collider['m_Points']['m_Paths']) == 1, 'Unsupported lever hit collider')
        offset = collider['m_Offset'];chain = [read(n) for n in hit['chain']]
        polygon = [transform_point([p['m_X']+offset['m_X'], p['m_Y']+offset['m_Y']], chain)
                   for p in collider['m_Points']['m_Paths'][0]]
        require(3 <= len(polygon) <= 16, 'Oversized lever polygon')
        turns = []
        for i, a in enumerate(polygon):
            b, c = polygon[(i+1) % len(polygon)], polygon[(i+2) % len(polygon)]
            turns.append((b[0]-a[0])*(c[1]-b[1])-(b[1]-a[1])*(c[0]-b[0]))
        require(all(t >= 0 for t in turns) or all(t <= 0 for t in turns), 'Lever hit polygon must be convex')
        ranges = [r for r in mapping['inventory'] if owner in r['owners'][1:] and r['type'] == 'BoxCollider2D']
        require(len(ranges) == 1, 'Unsupported lever range hierarchy')
        row = ranges[0];c = read(row['collider']);parts = row['components'][0]
        require(c['m_IsTrigger'] and c['m_Enabled'] and len(parts.get('PlayMakerFSM', [])) == 1, 'Unsupported lever range')
        range_fsm = decode_fsm(read(parts['PlayMakerFSM'][0])['m_Structure']['fsm'])
        require(signature(range_fsm) == SIGNATURES['Range'], 'Changed lever range FSM')
        require(variable(range_fsm, 'stringVariables', 'Bool Name') == 'In Range' and variable(range_fsm, 'stringVariables', 'FSM Name') == 'Switch Control', 'Changed lever range binding')
        center, size = c['m_Offset'], c['m_Size'];chain = [read(n) for n in row['chain']]
        points = [transform_point([center['m_X']+x*size['m_X']/2, center['m_Y']+y*size['m_Y']/2], chain)
                  for x,y in ((-1,-1),(1,-1),(1,1),(-1,1))]
        bounds = [min(p[0] for p in points), min(p[1] for p in points), max(p[0] for p in points), max(p[1] for p in points)]
        # Unity's serialized transform chain accumulates small rotation/scale
        # round-off even for this authored axis-aligned trigger.  Keep the
        # runtime Rect representation, but accept the bounded source error.
        require(all(any(abs(p[0]-bounds[x]) < 1e-3 for x in (0,2)) and any(abs(p[1]-bounds[y]) < 1e-3 for y in (1,3)) for p in points), 'Rotated lever range requires polygon support')
        gate_rows = [r for r in mapping['inventory'] if r['go'] == gate and r['type'] == 'BoxCollider2D']
        require(len(gate_rows) == 1, 'Missing source gate collider')
        persistent = pair['persistent']['persistentBoolData']
        scene = persistent['sceneName'] or mapping['scene']
        object_name = persistent['id'] or read(hit['go'])['m_Name']
        if profile:
            seconds = [wait(pair['fsm'], 'Hit'), wait(pair['gate_fsm'], 'Open'),
                       0., duration('Gate Open'), 0.]
            require(seconds[:3] == [.75, 1., 0.], 'Changed Ruins lever wait clocks')
        else:
            seconds = [wait(pair['fsm'], 'Hit'), wait(pair['gate_fsm'], 'Open Pause'), wait(pair['gate_fsm'], 'Dust'), duration('Door Open 1'), duration('Door Open 2')]
        require(all(math.isfinite(x) and 0 <= x < 30 for x in seconds) and
                seconds[0] > 0 and seconds[3] > 0, 'Invalid lever timing')
        result.append(dict(owner=identity(owner), gate=identity(gate), collider=identity(gate_rows[0]['collider']),
                           key=[key(scene), key(object_name)], range=bounds, polygon=polygon, seconds=seconds))
        if profile and profile.get('player_data'):
            result[-1]['player_data'] = key(profile['player_data'])
    require(len(result) <= 8, 'Too many source levers')
    return result, {r['collider'] for r in result}


def write_levers(rows, path):
    extended=any(row.get('pillar') or row.get('player_data') for row in rows)
    data = bytearray(struct.pack('<4sII', b'HKL1', 2 if extended else 1, len(rows)))
    for row in rows:
        data.extend(struct.pack('<6I9f', row['owner'], row['gate'], row['collider'], *row['key'], len(row['polygon']), *row['range'], *row['seconds']))
        if extended:
            data.extend(struct.pack('<4If', int(row.get('pillar',False)), *row.get('secondary_key',[0,0]), row.get('player_data',0), row.get('shake_fps',0)))
        for point in row['polygon']:
            data.extend(struct.pack('<2f', *point))
    path.write_bytes(data)
    return data


def require(condition, message):
    if not condition:
        raise ValueError(message)


def gate_artwork(mapping, source, read):
    """Admit the reviewed Mantis gate and Raising Pillar animation clips."""
    import copy
    import re
    manifest = source / 'tk2d/manifest.json'
    if not manifest.is_file():
        return [], {}
    supplement = json.loads(manifest.read_bytes())
    inventory = {row['go']: row for row in mapping['inventory']}
    profile, artwork = [], {}
    def import_clips(owner, library_path, names):
        row = inventory[owner]
        initial = row['sprite']
        match = re.fullmatch(r'(.*)-sprite-(\d+)\.json', initial)
        require(match is not None, 'Invalid gate sprite collection')
        collection_key, initial_id = match[1] + '.json', int(match[2])
        collection = read(collection_key)['m_Structure']
        definitions = collection['spriteDefinitions']
        material = definitions[initial_id]['materialId']
        library = read(library_path)['m_Structure']
        clips = {clip['name']: clip for clip in library['clips']}
        selected = []
        for name in names:
            clip = clips[name]
            require(clip['fps'] > 0 and (clip['wrapMode'] in (0,2) or
                    (clip['wrapMode']==6 and len(clip['frames'])==1)) and 0 < len(clip['frames']) <= 64,
                    'Unsupported source gate opening clip')
            keys = []
            for frame in clip['frames']:
                ref = frame['spriteCollection']
                require(ref['m_FileID'] == 0 and collection_key.endswith('-' + str(ref['m_PathID']) + '.json'),
                        'External gate animation collection is unresolved')
                sprite_id = frame['spriteId']
                require(0 <= sprite_id < len(definitions), 'Invalid gate animation sprite')
                definition = copy.deepcopy(definitions[sprite_id])
                require(not definition['complexGeometry'] and len(definition['positions']) == 4 and
                        len(definition['uvs']) == 4 and definition['materialId'] == material,
                        'Unsupported gate animation geometry/material')
                definition['materialId'] = 0
                key = match[1] + '-sprite-' + str(sprite_id) + '.json'
                artwork[key] = (source / mapping['sprites'][initial],
                                dict(_tk2d_definition=definition, m_PixelsToUnits=1))
                keys.append(key)
            selected.append(keys)
        return initial,selected,clips
    for pair in supplement.get('levers', []):
        pillar=pair.get('pillar',False)
        names=('Pillar Shake','Pillar Open','Pillar Opened') if pillar else SOURCE_PROFILES.get(mapping.get('scene'), {}).get('clips', ('Door Open 1','Door Open 2'))
        initial,selected,clips=import_clips(pair['gate'],pair['library'],names)
        entry=dict(gate=identity(pair['gate']), authored=initial, pillar=pillar,
                   first=len(selected[0]), frames=[initial] + sum(selected,[]))
        if pillar:
            switch_initial,switch_selected,switch_clips=import_clips(pair['owner'],pair['switch_library'],('Switch Idle','Switch Hit','Switch Activated'))
            require(len(selected[2])==1 and len(switch_selected[2])==1,'Unsupported pillar final pose')
            entry.update(switch_owner=identity(pair['owner']), switch_frames=[switch_initial]+sum(switch_selected,[]),
                         switch_idle=len(switch_selected[0]), switch_idle_fps=switch_clips['Switch Idle']['fps'],
                         switch_fps=switch_clips['Switch Hit']['fps'])
        profile.append(entry)
    return profile, artwork


def bind_gate_artwork(profile, by_sprite, draws):
    extended=any(gate.get('pillar') for gate in profile)
    data = bytearray(struct.pack('<4sI', b'HKA2' if extended else b'HKA1', len(profile)))
    for gate in profile:
        require(gate['frames'] and gate['first'] > 0, 'Gate binding requires original opening frames')
        indices = []
        for key in gate['frames']:
            tiles = by_sprite.get(key, [])
            require(len(tiles) == 1, 'Gate frame must occupy one whole atlas tile: ' + key)
            indices.append(tiles[0])
        selected = [draw for draw in draws if draw['owner'] == gate['gate']]
        require(len(selected) == 1 and selected[0]['sprite'] == indices[0],
                'Gate authored draw missing or split')
        data.extend(struct.pack('<3I', gate['gate'], gate['first'], len(indices)))
        data.extend(struct.pack('<' + 'I' * len(indices), *indices))
        if extended:
            switch=[by_sprite[key] for key in gate.get('switch_frames',[])]
            require(all(len(tiles)==1 for tiles in switch),'Switch frame must occupy one atlas tile')
            if switch:
                selected=[draw for draw in draws if draw['owner']==gate['switch_owner']]
                require(len(selected)==1 and selected[0]['sprite']==switch[0][0],'Switch authored draw missing')
            data.extend(struct.pack('<2I2f',len(switch),gate.get('switch_idle',0),
                                    gate.get('switch_idle_fps',0),gate.get('switch_fps',0)))
            data.extend(struct.pack('<'+'I'*len(switch),*(tiles[0] for tiles in switch)))
    return data
