"""Serialized TransitionPoint and Door Control data; no scene-name overrides."""
import math
import struct
from convert_terrain import transform_point
from inspect_fsm import decode_param


def _press_interaction(fsm, action_name):
    for state in fsm.get('states', []):
        if state['name'] != 'In Range':
            continue
        events = {edge['fsmEvent']['name'] for edge in state['transitions']
                  if edge['toState'] == 'Can Enter?'}
        data = state['actionData']
        starts = data['actionStartIndex'] + [len(data['paramName'])]
        for index, name in enumerate(data['actionNames']):
            if name != 'HutongGames.PlayMaker.Actions.' + action_name or not data['actionEnabled'][index]:
                continue
            for param in range(starts[index], starts[index + 1]):
                if data['paramName'][param] == 'wasPressed':
                    event = decode_param(data, param, fsm['dataVersion'])
                    if not isinstance(event, str):
                        raise ValueError(f'Unsupported Door Control {action_name} press event')
                    if event and event in events:
                        return True
    return False


def down_interaction(fsm):
    """Only admit an enabled, authored Down press leading to Can Enter?."""
    return _press_interaction(fsm, 'ListenForDown')


def _actions(fsm, state_name, action_name):
    for state in fsm.get('states', []):
        if state['name'] != state_name:
            continue
        data = state['actionData']
        starts = data['actionStartIndex'] + [len(data['paramName'])]
        for index, name in enumerate(data['actionNames']):
            if name.rsplit('.', 1)[-1] == action_name and data['actionEnabled'][index]:
                yield {data['paramName'][param]: decode_param(data, param, fsm['dataVersion'])
                       for param in range(starts[index], starts[index + 1])}


def departure_wait(fsm):
    """Only represent the authored half-second scaled Enter -> Change Scene wait."""
    states = [state for state in fsm.get('states', []) if state['name'] == 'Enter']
    waits = list(_actions(fsm, 'Enter', 'Wait'))
    if len(states) != 1 or len(waits) != 1:
        raise ValueError('Door Control requires one Enter departure Wait')
    wait = waits[0]
    time = wait.get('time', {})
    edges = [(edge['fsmEvent']['name'], edge['toState']) for edge in states[0]['transitions']]
    if (time.get('useVariable', True) or time.get('value') != .5 or
            wait.get('realTime') is not False or wait.get('finishEvent') != 'FINISHED' or
            edges != [('FINISHED', 'Change Scene')]):
        raise ValueError('Unsupported Door Control departure Wait')


def entry_pause(fsm):
    actions = list(_actions(fsm, 'Change Scene', 'BeginSceneTransition'))
    if len(actions) != 1:
        raise ValueError('Door Control requires one Change Scene transition')
    value = actions[0].get('entryDelay', {})
    variable = value.get('variable') or (value.get('name') if value.get('useVariable') else None)
    if value.get('useVariable') and not variable:
        raise ValueError('Unresolved Door Control Entry Pause')
    if variable:
        matches = [v for v in fsm.get('variables', {}).get('floatVariables', [])
                   if v['name'] == variable]
        if len(matches) != 1:
            raise ValueError('Unresolved Door Control Entry Pause')
        value = matches[0]
    delay = value.get('value')
    if isinstance(delay, bool) or not isinstance(delay, (int, float)) or not math.isfinite(delay) or delay < 0:
        raise ValueError('Invalid Door Control Entry Pause')
    return delay


def just_fade(fsm):
    """Decode the enabled Enter broadcast handled by CameraFade template 20521."""
    events = []
    for action in _actions(fsm, 'Enter', 'SendEventByName'):
        event = action['sendEvent']
        if event.get('useVariable') or not isinstance(event.get('value'), str):
            raise ValueError('Unresolved Door Control Enter broadcast')
        if event['value'] == 'DOOR ENTER':
            continue
        if (event['value'] not in ('FADE OUT', 'JUST FADE') or
                action['eventTarget']['target'] != 1 or
                action['eventTarget']['excludeSelf'].get('useVariable') or
                action['eventTarget']['excludeSelf'].get('value') is not False or
                action['delay'].get('useVariable') or action['delay'].get('value') != 0 or
                action['everyFrame'] is not False):
            raise ValueError('Unsupported Door Control Enter fade broadcast')
        events.append(event['value'])
    if len(events) != 1:
        raise ValueError('Door Control requires exactly one enabled Enter fade broadcast')
    return events[0] == 'JUST FADE'


def startup(fsm):
    """Decode the supported Door Control startup graph, never a door-name policy."""
    states = {s['name']: s for s in fsm['states']}
    start = fsm.get('startState')
    if start == 'Idle':
        return None
    if start not in ('Init', 'Wait for enter scene') or not fsm.get('RestartOnEnable'):
        raise ValueError('Unsupported Door Control startup / restart policy')

    def edge(state, event, target):
        matches = [t['toState'] for t in states[state]['transitions']
                   if t['fsmEvent']['name'] == event]
        if matches != [target]:
            raise ValueError('Unsupported Door Control startup transition')

    def action(state, name):
        data = states[state]['actionData']
        enabled = [n.rsplit('.', 1)[-1] for n, on in
                   zip(data['actionNames'], data['actionEnabled']) if on]
        if enabled != [name]:
            raise ValueError('Unsupported Door Control startup actions')
        return next(_actions(fsm, state, name))

    def wait(state, target):
        value = action(state, 'Wait')
        time = value['time']
        if (value['realTime'] or time.get('useVariable') or 'variable' in time or
                value['finishEvent'] != 'FINISHED' or 'value' not in time or
                not isinstance(time['value'], (float, int)) or
                not math.isfinite(time['value']) or time['value'] < 0):
            raise ValueError('Unsupported Door Control startup Wait')
        edge(state, 'FINISHED', target)
        return time['value']

    try:
        pause = 0
        if start == 'Wait for enter scene':
            if action(start, 'WaitForFinishedEnteringScene')['sendEvent'] != 'FINISHED':
                raise ValueError('Unsupported Door Control scene-entry event')
            edge(start, 'FINISHED', 'Pause')
            pause = wait('Pause', 'Init')
        edge('Init', 'FINISHED', 'Crossroads?')
        init = states['Init']['actionData']
        if any(on and name.rsplit('.', 1)[-1] not in
               # The prompt group (CreateObject 'Prompt Control', SetFsmString
               # 'Enter', Activate/DestroyObject) only builds the arrow prompt.
               ('GetOwner', 'GetHero', 'SetGameObject', 'FindChild', 'FindGameObject',
                'CreateObject', 'SetFsmString', 'ActivateGameObject', 'DestroyObject')
               for name, on in zip(init['actionNames'], init['actionEnabled'])):
            raise ValueError('Unsupported Door Control Init action')
        test = action('Crossroads?', 'BoolTest')
        if test != dict(boolVariable={'variable': 'Crossroads Ascent'},
                        isTrue='WAIT', isFalse='FINISHED', everyFrame=False):
            raise ValueError('Unsupported Door Control ascent test')
        values = [v['value'] for v in fsm['variables']['boolVariables']
                  if v['name'] == 'Crossroads Ascent']
        if len(values) != 1 or type(values[0]) is not bool:
            raise ValueError('Unresolved Door Control ascent variable')
        ascent = values[0]
        edge('Crossroads?', 'FINISHED', 'Normal Pause')
        normal = wait('Normal Pause', 'Idle')
        visited = unvisited = 0
        if ascent:
            edge('Crossroads?', 'WAIT', 'Visited Check')
            data = states['Visited Check']['actionData']
            enabled = [n.rsplit('.', 1)[-1] for n, on in
                       zip(data['actionNames'], data['actionEnabled']) if on]
            reads = list(_actions(fsm, 'Visited Check', 'GetPlayerDataBool'))
            tests = list(_actions(fsm, 'Visited Check', 'BoolTest'))
            if (enabled != ['GetPlayerDataBool', 'BoolTest'] or len(reads) != 1 or
                    reads[0]['boolName'].get('useVariable') or
                    reads[0]['boolName'].get('value') != 'visitedCrossroads' or
                    tests != [dict(boolVariable=reads[0]['storeValue'],
                                   isTrue='VISITED', isFalse='UNVISITED', everyFrame=False)]):
                raise ValueError('Unsupported Door Control visited test')
            edge('Visited Check', 'VISITED', 'Visited')
            edge('Visited Check', 'UNVISITED', 'Unvisited')
            visited, unvisited = wait('Visited', 'Idle'), wait('Unvisited', 'Idle')
        return dict(flags=1 | (2 if start == 'Wait for enter scene' else 0) |
                    (4 if ascent else 0), pause=pause, normal=normal,
                    visited=visited, unvisited=unvisited)
    except (KeyError, StopIteration, TypeError) as error:
        raise ValueError('Incomplete Door Control startup graph') from error


def prompt_binding(fsm):
    """Follow the enabled Show/FindChild binding; variable presence is not Show."""
    shows = list(_actions(fsm, 'In Range', 'ShowPromptMarker'))
    if not shows:
        return None
    if len(shows) != 1:
        raise ValueError('Door prompt requires exactly one enabled ShowPromptMarker')
    show = shows[0]

    def variable(value):
        if not value.get('useVariable') or not value.get('name'):
            raise ValueError('Unsupported Door prompt GameObject binding')
        return value['name']

    def string(value):
        if value.get('useVariable'):
            values = [v['value'] for v in fsm['variables']['stringVariables']
                      if v['name'] == value['name']]
            if len(values) != 1:
                raise ValueError('Unresolved Door prompt string variable')
            return values[0]
        return value['value']

    prefab = show['prefab']
    if prefab.get('useVariable'):
        raise ValueError('Unsupported Door prompt prefab variable')
    if not prefab['value']['m_PathID']:
        return None  # ShowPromptMarker.OnEnter does nothing without a prefab.
    spawn = variable(show['spawnPoint'])
    lookups = [action for action in _actions(fsm, 'Init', 'FindChild')
               if variable(action['storeResult']) == spawn]
    if len(lookups) != 1:
        raise ValueError('Door prompt requires exactly one enabled spawn FindChild')
    lookup = lookups[0]
    owner = lookup['gameObject']
    if owner['ownerOption'] == 1:
        owner_name = variable(owner['gameObject'])
        owners = [action for action in _actions(fsm, 'Init', 'GetOwner')
                  if variable(action['storeGameObject']) == owner_name]
        if len(owners) != 1:
            raise ValueError('Door prompt FindChild must use the controller owner')
    elif owner['ownerOption'] != 0:
        raise ValueError('Unsupported Door prompt FindChild owner')
    marker_name, label = string(lookup['childName']), string(show['labelName'])
    if not isinstance(marker_name, str) or not marker_name or '/' in marker_name:
        raise ValueError('Unsupported Door prompt child path')
    if not isinstance(label, str) or not label:
        raise ValueError('Unsupported Door prompt empty label')
    return dict(marker_name=marker_name, label=label)


def prompt_marker(controller, read, transform, marker_name='Prompt Marker'):
    """Resolve Door Control's direct child lookup without inventing geometry."""
    prefix = controller.removesuffix('.json').rsplit('-', 1)[0]
    markers = []
    for reference in transform.get('m_Children', []):
        if reference['m_FileID']:
            raise ValueError('Door prompt child references another collection')
        child = read(f"{prefix}-{reference['m_PathID']}.json")
        owner = child['m_GameObject']
        if owner['m_FileID']:
            raise ValueError('Door prompt owner references another collection')
        if read(f"{prefix}-{owner['m_PathID']}.json")['m_Name'] == marker_name:
            markers.append(child)
    if len(markers) != 1:
        raise ValueError(f'Door prompt requires exactly one exported {marker_name} child')
    return markers[0]


def properties(data, local, read, chain):
    binding = None
    down = False
    interact = False
    startup_data = None
    quick_fade = False
    pause = 0
    controller = ''
    for key in local.get('PlayMakerFSM', []):
        component = read(key)
        if not component.get('m_Enabled', 1):
            continue
        fsm = component.get('m_Structure', {}).get('fsm', {})
        if fsm.get('name') == 'Door Control':
            if controller:
                raise ValueError('Multiple enabled Door Control FSMs')
            controller = key
            binding = prompt_binding(fsm)
            down = down_interaction(fsm)
            interact = _press_interaction(fsm, 'ListenForUp')
            if interact:
                startup_data = startup(fsm)
                departure_wait(fsm)
                quick_fade = just_fade(fsm)
                pause = entry_pause(fsm)
            if (down or binding) and not interact:
                raise ValueError('Unsupported Door Control without enabled Up listener')
    flags = (int(data.get('isADoor', False)) |
             (2 if interact else 0) |
             (4 if data.get('alwaysEnterRight') else 0) |
             (8 if data.get('alwaysEnterLeft') else 0) |
             (16 if data.get('dontWalkOutOfDoor') else 0) |
             (32 if down else 0) | (64 if quick_fade else 0) |
             (128 if data.get('customFade') else 0))
    position = transform_point([0, 0], chain)
    prompt_position = position
    if binding:
        if not chain:
            raise ValueError('Door prompt requires exported Transform ancestry')
        marker = prompt_marker(controller, read, chain[0], binding['marker_name'])
        prompt_position = transform_point([0, 0], [marker, *chain])
    return dict(flags=flags, position=position,
                prompt=binding['label'] if binding else '',
                prompt_position=prompt_position, startup=startup_data, entry_pause=pause)


def write(path, gates):
    version3 = any(gate.get('entry_pause', 0) or gate['flags'] & 128 for gate in gates)
    version2 = version3 or any(gate.get('startup') for gate in gates)
    binary = bytearray(struct.pack('<4sI', b'HKD3' if version3 else b'HKD2' if version2 else b'HKD1', len(gates)))
    for gate in gates:
        prompt = gate['prompt'].encode('ascii')
        if len(prompt) >= 32:
            raise ValueError('Door prompt too long')
        binary += struct.pack('<I4f32s', gate['flags'], *gate['position'],
                              *gate['prompt_position'], prompt)
        if version2:
            value = gate.get('startup')
            binary += struct.pack('<I4f', *(tuple(value[k] for k in
                                  ('flags', 'pause', 'normal', 'visited', 'unvisited'))
                                  if value else (0, 0, 0, 0, 0)))
        if version3:
            pause = gate.get('entry_pause', 0)
            if isinstance(pause, bool) or not isinstance(pause, (int, float)) or not math.isfinite(pause) or pause < 0:
                raise ValueError('Invalid Door Control Entry Pause')
            binary += struct.pack('<f', pause)
    path.write_bytes(binary)
