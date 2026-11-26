"""Recognize effect-only spike reactions without executing their visual FSM.

Build22529139 Fungus1_05 level134-6184 creates Hit Effect before rotating/scaling
that variable. Admission only describes the stationary damage owner; source
slash-effect presentation remains a separate adapter responsibility.
"""
PREFIX = 'HutongGames.PlayMaker.Actions.'


ANGLES = {'Hit Left': (30,70), 'Hit Right': (110,150),
          'Hit Down': (45,115), 'Hit Up': (225,315)}
ROTATIONS = {'Hit Left': (0,90,0), 'Hit Right': (0,270,180),
             'Hit Down': (270,90,0), 'Hit Up': (90,90,0)}


def constant(parameter, value):
    return not parameter['useVariable'] and parameter['value'] == value


def source_spawn(kind, state, p):
    # These PPtrs belong specifically to level134-6184; the caller pins it.
    prefab = p['gameObject']
    if prefab['useVariable']:
        return False
    pointer = (prefab['value']['m_FileID'], prefab['value']['m_PathID'])
    point = p['spawnPoint']
    if kind == 'CreateObject':
        stored = p['storeObject']
        if not stored['useVariable'] or not constant(p['networkInstantiate'], False) or not constant(p['networkGroup'], 0):
            return False
        if state == 'Get Direction':
            return (pointer == (4,4126) and stored['name'] == 'Audio Player'
                    and point['useVariable'] and point['name'] == 'Hero')
        if state not in ANGLES or point['useVariable'] or point['value']['m_PathID'] or point['value'].get('m_FileID', 0):
            return False
        if p['position'].get('variable') != 'Effect Origin':
            return False
        if pointer == (13,67) and stored['name'] == 'Hit Effect':
            return p['rotation']['useVariable']
        if pointer != (14,2) or stored['name']:
            return False
        expected = dict(zip(('x','y','z'), ROTATIONS[state]))
        return constant(p['rotation'], expected)
    if state not in ANGLES or pointer != (4,5603) or point['useVariable'] or point['value']['m_PathID'] or point['value'].get('m_FileID',0):
        return False
    if p['position'].get('variable') != 'Effect Origin':
        return False
    lo,hi=ANGLES[state]
    expected = dict(spawnMin=2,spawnMax=3,speedMin=14,speedMax=18,
                    angleMin=lo,angleMax=hi,originVariationX=0,originVariationY=.5)
    return (all(constant(p[key],value) for key,value in expected.items())
            and all(p[key]['useVariable'] and not p[key]['name'] and not p[key]['value']
                    for key in ('FSM','FSMEvent')))


def spike_slash_controller(fsm):
    try:
        if (fsm['name'] != 'Slash Reaction' or fsm['start'] != 'Init'
                or fsm.get('globalTransitions', [])):
            return False
        states = fsm['states']
        names = {'Init', 'Detect', 'Get Direction', 'Hit Left', 'Hit Right',
                 'Hit Down', 'Hit Up', 'Pause'}
        if len(states) != len(names) or {s['name'] for s in states} != names:
            return False
        allowed = {PREFIX + name for name in (
            'Trigger2dEvent', 'RandomFloat', 'CreateObject', 'SetAudioPitch',
            'AudioPlay', 'GetFsmFloat', 'GetPosition', 'FloatSwitch', 'FloatAdd',
            'SetVector3XYZ', 'SetRotation', 'SetScale', 'Wait', 'FlingObjectsFromGlobalPool')}
        for state in states:
            created_effect = False
            for action in state['actions']:
                if not action['enabled'] or action['type'] not in allowed:
                    return False
                parameters = action['parameters']
                kind = action['type'].removeprefix(PREFIX)
                if kind in ('CreateObject', 'FlingObjectsFromGlobalPool') and not source_spawn(kind, state['name'], parameters):
                    return False
                if kind == 'CreateObject':
                    stored = parameters.get('storeObject', {})
                    if stored.get('name') == 'Hit Effect':
                        prefab = parameters['gameObject']
                        if (prefab['useVariable'] or prefab['value']['m_PathID'] <= 0
                                or not stored['useVariable']):
                            return False
                        created_effect = True
                if kind in ('SetRotation', 'SetScale'):
                    target = parameters['gameObject']
                    if (not created_effect or target['ownerOption'] != 1
                            or not target['gameObject']['useVariable']
                            or target['gameObject']['name'] != 'Hit Effect'
                            or parameters['everyFrame'] or parameters['lateUpdate']):
                        return False
        return True
    except (KeyError, TypeError):
        return False
