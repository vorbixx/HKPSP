"""Recognize the source acid-volume policy already implemented by RoomRuntime.

Build22529139: Fungus1_03 level131-5239 / Fungus1_17 level146-4128.
The original actions read hasAcidArmour and set only self DamageHero to zero.
Input is inspect_fsm.decode_fsm output; unknown policies stay unsupported.
"""
PREFIX = 'HutongGames.PlayMaker.Actions.'


def acid_armour_controller(fsm):
    try:
        if (fsm['name'] != 'Acid Armour Check' or fsm['start'] != 'Pause'
                or fsm.get('globalTransitions', []) or len(fsm['states']) != 4):
            return False
        states = {state['name']: state for state in fsm['states']}
        actions = {'Pause': [PREFIX + 'NextFrameEvent'],
                   'Check': [PREFIX + 'PlayerDataBoolTest'],
                   'Disable': ['SetDamageHeroAmount'], 'Recheck': []}
        transitions = {'Pause': [('FINISHED', 'Check')],
                       'Check': [('DISABLE', 'Disable'), ('GET ACID ARMOUR', 'Recheck')],
                       'Disable': [], 'Recheck': [('FINISHED', 'Check')]}
        if set(states) != set(actions):
            return False
        for name, expected in actions.items():
            state = states[name]
            if ([a['type'] for a in state['actions']] != expected
                    or not all(a['enabled'] for a in state['actions'])
                    or [(t['fsmEvent']['name'], t['toState']) for t in state['transitions']] != transitions[name]):
                return False
        check = states['Check']['actions'][0]['parameters']
        target = check['gameObject']
        disable = states['Disable']['actions'][0]['parameters']
        return (target['ownerOption'] == 1 and target['gameObject']['useVariable']
                and target['gameObject']['name'] == 'GameManager'
                and check['boolName']['value'] == 'hasAcidArmour' and not check['boolName']['useVariable']
                and check['isTrue'] == 'DISABLE' and check['isFalse'] == ''
                and disable['target']['ownerOption'] == 0
                and disable['damageDealt']['value'] == 0 and not disable['damageDealt']['useVariable']
                and states['Pause']['actions'][0]['parameters']['sendEvent'] == 'FINISHED')
    except (KeyError, TypeError):
        return False
