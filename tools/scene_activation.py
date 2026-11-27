"""Compile source-backed scene activation predicates to the HKA1 runtime bank.

The compiler only models initial scene state: direct PlayerData deactivators and
PlayMaker initial bool tests whose branches activate known children/components.
Unknown actions remain visible in the inventory instead of being guessed.
"""
import hashlib
import json
import os
import struct
import shutil
from pathlib import Path
from inspect_fsm import decode_param


MAX_NODES = 2048
# Set by the player builder (tools/psp/hkpsp_build.py). Hash-verified source records are
# shared between the compilers one scene conversion builds (metadata and export_scene).
SHARE_VERIFIED_READS = os.environ.get('HKPSP_FAST_CHECKPOINT') == '1'
_VERIFIED_READS = {}
_DIRECTORY_NAMES = {}


def _source_exists(path):
    """Player builds: one directory listing answers every lookup in that directory.

    Source records are read-only during a conversion; 9k stat calls per room otherwise."""
    if not SHARE_VERIFIED_READS:
        return path.exists()
    parent = os.path.normcase(os.path.abspath(path.parent))
    names = _DIRECTORY_NAMES.get(parent)
    if names is None:
        try:
            names = frozenset(os.path.normcase(name) for name in os.listdir(parent))
        except OSError:
            names = frozenset()
        _DIRECTORY_NAMES[parent] = names
    return os.path.normcase(path.name) in names
MAX_TARGETS = 8192
TARGET_KINDS = {'visual': 0, 'collider': 1, 'gate': 2, 'spawn': 3, 'hazard': 4, 'acid_water': 5}
DEACTIVATORS = ('DeactivateIfPlayerdataFalse', 'DeactivateIfPlayerdataTrue')


def fnv1a32(text):
    value = 2166136261
    for byte in text.encode('utf-8'):
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def owner_id(source_ref):
    parts = Path(source_ref).stem.rsplit('-', 1)
    # Synthetic/static asset fixtures need no serialized Unity owner ID.
    return int(parts[1]) if len(parts) == 2 and parts[1].isdigit() else 0


class ActivationCompiler:
    """Source maps and extracted Unity records -> activation expressions."""
    def __init__(self, sources, maps):
        self.sources = [Path(os.path.abspath(p)) for p in sources]
        self.maps = [m if isinstance(m, dict) else json.loads(Path(m).read_text(encoding='utf-8')) for m in maps]
        self.nodes = []
        self._node_ids = {}
        self.owner_conditions = {}
        self.collider_conditions = {}
        self.renderer_conditions = {}
        self.inventory = []
        self._cache = {}
        self._resolved_refs = {}
        self._source_hashes = {}
        self._canonical_hashes = None
        self._canonical_dirs = {}
        self._persistent_fsms = {}
        for source, mapping in zip(self.sources, self.maps):
            # Source roots and references must use the same lexical spelling,
            # including worktree junctions. No filesystem traversal per record.
            self._source_hashes.update({Path(os.path.abspath(source / k)): v
                                        for k, v in mapping.get('source_sha256', {}).items()})
        self._scan_components()
        self._scan_persistent_items()
        self._scan_fsm_tests()

    def _canonical_path(self, path):
        parent = path.parent
        if parent not in self._canonical_dirs:
            self._canonical_dirs[parent] = Path(os.path.realpath(parent))
        if path.is_symlink():
            return Path(os.path.realpath(path))
        return self._canonical_dirs[parent] / path.name

    def _read(self, ref):
        path = Path(ref)
        resolved = self._resolved_refs.get(path)
        if resolved is None:
            if not path.is_absolute():
                path = next((s / ref for s in self.sources if _source_exists(s / ref)), self.sources[0] / ref)
            path = Path(os.path.abspath(path))
            resolved = path
            if not any(path.is_relative_to(source) for source in self.sources):
                # An external absolute alias is rare. Resolve its directory once,
                # retaining hash validation without resolving every source entry.
                resolved = self._canonical_path(path)
                if self._canonical_hashes is None:
                    self._canonical_hashes = {self._canonical_path(key): value
                                              for key, value in self._source_hashes.items()}
        path = resolved
        if path not in self._cache:
            expected = self._source_hashes.get(path)
            if expected is None and self._canonical_hashes is not None:
                expected = self._canonical_hashes.get(path)
            token = None
            if SHARE_VERIFIED_READS and expected:
                stat = path.stat()
                token = (expected, stat.st_size, stat.st_mtime_ns)
            shared = _VERIFIED_READS.get(path) if token else None
            if shared is not None and shared[0] == token:
                # Player builds: a second compiler in this process reuses records already
                # checked against the same source hash, while size and mtime are unchanged,
                # instead of re-reading and re-parsing them.
                self._cache[path] = shared[1]
            else:
                raw = path.read_bytes()
                if expected and hashlib.sha256(raw).hexdigest() != expected:
                    raise ValueError('Changed activation source: ' + str(path))
                self._cache[path] = json.loads(raw)
                if token:
                    _VERIFIED_READS[path] = (token, self._cache[path])
        self._resolved_refs[Path(ref)] = path
        return self._cache[path]

    def node(self, op, a=0, b=0):
        key = (op, a & 0xffffffff, b & 0xffffffff)
        if key not in self._node_ids:
            if len(self.nodes) >= MAX_NODES:
                raise ValueError('Activation node capacity exceeded')
            self._node_ids[key] = len(self.nodes)
            self.nodes.append(key)
        return self._node_ids[key]

    def constant(self, value):
        return self.node(0, int(bool(value)), 0)

    def saved(self, name, value=True):
        if not name:
            raise ValueError('Empty PlayerData field in activation source')
        return self.node(1 if value else 2, fnv1a32(name), 1)

    def combine(self, expressions):
        expressions = list(dict.fromkeys(expressions))
        if not expressions:
            return None
        result = expressions[0]
        for expression in expressions[1:]:
            result = self.node(7, result, expression)
        return result

    def disjoin(self, expressions):
        expressions = list(dict.fromkeys(expressions))
        if not expressions:
            return None
        result = expressions[0]
        for expression in expressions[1:]:
            result = self.node(8, result, expression)
        return result

    def condition_for_owners(self, owners):
        """Return target node for an owner path, None when purely static."""
        return self.combine(expr for owner in owners for expr in self.owner_conditions.get(owner_id(owner), ()))

    def _initial_component_active(self, target, kind, objects):
        for mapping in self.maps:
            for row in mapping.get('inventory', []):
                if owner_id(row.get('go', '')) != target:
                    continue
                components = row.get('components', {})
                if isinstance(components, list):
                    components = components[0] if components else {}
                for name, refs in components.items():
                    if ((kind == 'collider' and name.endswith('Collider2D')) or
                            (kind == 'renderer' and name in ('SpriteRenderer', 'MeshRenderer'))):
                        return bool(self._read(refs[0]).get('m_Enabled', True))
        return bool(objects.get(target, {}).get('m_IsActive', True))

    def _inventory_component_refs(self):
        refs = {}
        for mapping in self.maps:
            for components in mapping.get('owner_components', {}).values():
                for kind, names in components.items():
                    refs.setdefault(kind, set()).update(names)
            for row in mapping.get('inventory', []):
                components = row.get('components', {})
                if isinstance(components, list):
                    for group in components:
                        for kind, names in group.items():
                            refs.setdefault(kind, set()).update(names)
                else:
                    for kind, names in components.items():
                        refs.setdefault(kind, set()).update(names)
        return refs

    def _scan_components(self):
        refs = self._inventory_component_refs()
        for kind in DEACTIVATORS:
            for ref in sorted(refs.get(kind, ())):
                data = self._read(ref)
                structure = data.get('m_Structure', {})
                field = structure.get('boolName', '')
                go = data.get('m_GameObject', {}).get('m_PathID')
                go = int(go or 0)
                record = dict(status='supported' if field and go else 'unhandled', kind=kind,
                              field=field, source=ref, owner=go)
                if field and go:
                    # DeactivateIfFalse disables when false; the object is live
                    # exactly when the PlayerData boolean is true. The True
                    # variant has the inverse condition.
                    expr = self.saved(field, kind == 'DeactivateIfPlayerdataFalse')
                    self.owner_conditions.setdefault(go, []).append(expr)
                    record['node'] = expr
                else:
                    record['reason'] = 'missing boolName or owner reference'
                self.inventory.append(record)

    def _scan_persistent_items(self):
        refs = self._inventory_component_refs()
        callbacks = ('Breakable', 'DreamPlant', 'DreamPlantOrb', 'HealthCocoon', 'HealthManager', 'VinePlatform')
        fsms = [(ref, self._read(ref)) for ref in sorted(refs.get('PlayMakerFSM', ()))]
        scenes = {mapping.get('scene') for mapping in self.maps if mapping.get('scene')}
        associations = {}
        for ref in sorted(refs.get('PersistentBoolItem', ())):
            data = self._read(ref)
            owner = int(data.get('m_GameObject', {}).get('m_PathID') or 0)
            structure = data.get('m_Structure', {})
            persistent = structure.get('persistentBoolData', {})
            row = dict(status='unhandled', kind='PersistentBoolItem', source=ref, owner=owner,
                       reason='no supported startup Activated branch')
            self.inventory.append(row)
            if structure.get('semiPersistent'):
                row['reason'] = 'semi-persistent scene data needs death/reset lifetime'
                continue
            callback = next((kind for kind in callbacks for callback_ref in refs.get(kind, ())
                             if self._read(callback_ref).get('m_GameObject', {}).get('m_PathID') == owner), None)
            if callback:
                row['reason'] = 'scene data uses ' + callback + ' callback, not FSM Activated'
                continue
            candidates = [(fsm_ref, fsm_data['m_Structure']['fsm']) for fsm_ref, fsm_data in fsms
                          if fsm_data.get('m_GameObject', {}).get('m_PathID') == owner
                          and any(v.get('name') == 'Activated' for v in fsm_data.get('m_Structure', {})
                                  .get('fsm', {}).get('variables', {}).get('boolVariables', []))]
            if len(candidates) != 1:
                row['reason'] = 'persistent owner has no unique Activated FSM'
                continue
            fsm_ref, fsm = candidates[0]
            initial = next(v.get('value') for v in fsm['variables']['boolVariables'] if v.get('name') == 'Activated')
            if persistent.get('activated') or initial:
                row['reason'] = 'initially true scene data needs explicit false save entries'
                continue
            # PersistentBoolItem.SetMyID uses the GameObject's name and base
            # scene name when the serialized identity is empty.
            collection = Path(ref).stem.rsplit('-', 1)[0]
            obj = self._read(collection + '-' + str(owner) + '.json')
            if not obj.get('m_IsActive', True):
                row['inactive_owner'] = True
            scene = persistent.get('sceneName') or (next(iter(scenes)) if len(scenes) == 1 else None)
            identity = persistent.get('id') or obj.get('m_Name')
            if not scene or not identity:
                row['reason'] = 'persistent scene/object identity is absent'
                continue
            if not persistent.get('sceneName'):
                for suffix in ('_boss_defeated', '_boss', '_preload'):
                    if scene.lower().endswith(suffix):
                        scene = scene[:-len(suffix)]
                        break
            row.update(scene=scene, field=identity)
            associations.setdefault(fsm_ref, []).append(row)
        for fsm_ref, rows in associations.items():
            if len(rows) == 1:
                self._persistent_fsms[fsm_ref] = rows[0]
            else:
                # Component callback order cannot establish which saved value
                # wins when several components write the same FSM variable.
                for row in rows:
                    row['reason'] = 'multiple persistent components share Activated variable'

    @staticmethod
    def _decode_params(action_data, index):
        names = action_data.get('actionNames', [])
        starts = action_data.get('actionStartIndex', [])
        if index >= len(starts):
            return {}
        begin = starts[index]
        end = starts[index + 1] if index + 1 < len(starts) else len(action_data.get('paramName', []))
        result = {}
        skip_until = begin
        for pos in range(begin, end):
            if pos < skip_until:
                continue
            name = action_data.get('paramName', [])[pos]
            kind = action_data.get('paramDataType', [])[pos]
            data_pos = action_data.get('paramDataPos', [])[pos]
            size = action_data.get('paramByteDataSize', [])[pos]
            if kind in (12, 16, 0, 7):
                # Reuse the versioned PlayMaker decoder for integers/arrays.
                # Array elements are records of their own, not action fields.
                version = action_data.get('version', 1)
                if kind == 16 and not size and 'fsmIntParams' in action_data:
                    version = 2
                value = decode_param(action_data, pos, version)
                if kind == 12:
                    value = value['elements']
                    skip_until = pos + len(value) + 1
                result[name] = value
            elif kind == 20:  # FsmOwnerDefault parameter
                values = action_data.get('fsmOwnerDefaultParams', [])
                result[name] = values[data_pos] if data_pos < len(values) else None
            elif kind == 18:  # FsmString variable/constant
                values = action_data.get('fsmStringParams', [])
                result[name] = values[data_pos] if data_pos < len(values) else None
            elif kind == 17:  # FsmBool parameter
                # PlayMaker v1 stores [value, useVariable, UTF-8 name] in
                # byteData (see inspect_fsm.py), including two-byte literals.
                raw = bytes(action_data.get('byteData', [])[data_pos:data_pos + size])
                if len(raw) >= 2:
                    result[name] = (dict(value=bool(raw[0]), useVariable=True,
                                         name=raw[2:].decode('utf-8', errors='replace').rstrip('\0'))
                                    if raw[1] else bool(raw[0]))
                else:
                    values = action_data.get('fsmBoolParams', [])
                    value = values[data_pos] if data_pos < len(values) else None
                    result[name] = (bool(value.get('value'))
                                    if isinstance(value, dict) and not value.get('useVariable')
                                    else value)
            elif kind == 19:  # FsmGameObject variable reference (FindChild.storeResult)
                values = action_data.get('fsmGameObjectParams', [])
                result[name] = values[data_pos] if data_pos < len(values) else None
            elif kind == 23:  # literal string stored in the byte stream
                raw = bytes(action_data.get('byteData', [])[data_pos:data_pos + size])
                if raw:
                    result[name] = raw.decode('utf-8', errors='replace').rstrip('\0')
                else:
                    values = action_data.get('stringParams', action_data.get('fsmStringParams', []))
                    result[name] = values[data_pos] if data_pos < len(values) else ''
            elif kind == 1:  # serialized bool literal in the byte stream
                raw = action_data.get('byteData', [])[data_pos:data_pos + size]
                result[name] = bool(raw and raw[0])
            else:
                result[name] = None
        return result

    def _scan_fsm_tests(self):
        refs = self._inventory_component_refs().get('PlayMakerFSM', ())
        if not refs:
            return
        children, objects = {}, {}
        for mapping in self.maps:
            for row in mapping.get('inventory', []):
                owners = row.get('owners', [])
                for ref in owners:
                    obj = self._read(ref)
                    path_id = int(obj.get('m_PathID') or owner_id(ref))
                    objects[path_id] = obj
                for child, parent in zip(owners, owners[1:]):
                    child_id = int(self._read(child).get('m_PathID') or owner_id(child))
                    parent_id = int(self._read(parent).get('m_PathID') or owner_id(parent))
                    children.setdefault(parent_id, []).append(child_id)
        # Resolve external startup activation before composing persistence's
        # suppression of an object enabled by another FSM.
        for fsm_ref in sorted(refs, key=lambda ref: (ref in self._persistent_fsms, ref)):
            data = self._read(fsm_ref)
            fsm = data.get('m_Structure', {}).get('fsm', {})
            states = {s.get('name'): s for s in fsm.get('states', [])}
            for entry in states.values():
                entry.get('actionData', {}).setdefault('version', fsm.get('dataVersion', 1))
            owner = int(data.get('m_GameObject', {}).get('m_PathID') or 0)
            persistent = self._persistent_fsms.get(fsm_ref)
            def next_static_state(current):
                transitions = current.get('transitions', [])
                event = 'FINISHED'
                if len(transitions) != 1 or transitions[0].get('fsmEvent', {}).get('name') != event:
                    ad = current.get('actionData', {})
                    if len(ad.get('actionNames', [])) != 1 or not ad['actionNames'][0].endswith('.BoolTest'):
                        return None
                    params = self._decode_params(ad, 0)
                    value = params.get('boolVariable')
                    if isinstance(value, dict) and value.get('useVariable'):
                        # Only fold a serialized configuration bool when this
                        # FSM contains no actions that could write variables.
                        read_only = {'NextFrameEvent', 'GetOwner', 'PlayerDataBoolTest', 'BoolTest',
                                     'ActivateGameObject', 'ActivateAllChildren'}
                        if any(a.rsplit('.', 1)[-1] not in read_only for s in states.values()
                               for a in s.get('actionData', {}).get('actionNames', [])):
                            return None
                        value = next((v.get('value') for v in fsm.get('variables', {}).get('boolVariables', [])
                                      if v.get('name') == value.get('name')), None)
                    if not isinstance(value, bool) or params.get('everyFrame') is not False:
                        return None
                    event = params.get('isTrue' if value else 'isFalse')
                transition = next((t for t in transitions if t.get('fsmEvent', {}).get('name') == event), None)
                return states.get(transition.get('toState')) if transition else None
            def child_of(parent_id, name):
                return next((child for child in children.get(parent_id, ())
                             if objects.get(child, {}).get('m_Name') == name), 0)
            # Only inspect startup states. A predicate in a dialogue or later
            # control state does not describe the scene's initial activation.
            reachable = {fsm.get('startState')}
            pending = list(reachable)
            startup_order = []
            while pending:
                current = states.get(pending.pop())
                if not current:
                    continue
                startup_order.append(current['name'])
                transitions = [t for t in current.get('transitions', [])
                               if t.get('fsmEvent', {}).get('name') == 'FINISHED']
                # FINISHED is unconditional only when it is the state's sole
                # transition. Do not traverse conditional/event-driven branches.
                if len(current.get('transitions', [])) == 1 and len(transitions) == 1:
                    dest = transitions[0].get('toState')
                    if dest in states and dest not in reachable:
                        reachable.add(dest)
                        pending.append(dest)
            test_kinds = ('.PlayerDataBoolTest', '.PlayerDataBoolTrueAndFalse', '.IntCompare', '.IntSwitch')
            if persistent:
                test_kinds += ('.BoolTest',)
            def string_value(param):
                if isinstance(param, str):
                    return param
                if not isinstance(param, dict):
                    return None
                name = param.get('variable') or (param.get('name') if param.get('useVariable') else None)
                if name:
                    if any(action.endswith(('.SetStringValue', '.GetPlayerDataString', '.GetStringValue',
                                             '.StringAppend', '.StringReplace'))
                           for startup_name in reachable
                           for action in states.get(startup_name, {}).get('actionData', {}).get('actionNames', [])):
                        return None
                    return next((v.get('value') for v in fsm.get('variables', {}).get('stringVariables', [])
                                 if v.get('name') == name), None)
                return param.get('value')
            int_fields = {}
            def preceding_integer_fields(startup_ad, before, incoming=None):
                fields = dict(incoming or {})
                for getter_i, getter_name in enumerate(startup_ad.get('actionNames', [])[:before]):
                    getter = self._decode_params(startup_ad, getter_i)
                    if getter_name.endswith('.GetPlayerDataInt'):
                        dest = getter.get('storeValue')
                        if isinstance(dest, dict):
                            name = dest.get('variable') or dest.get('name')
                            if name:
                                fields[name] = string_value(getter.get('intName'))
                    elif not getter_name.endswith(('.IntCompare', '.IntSwitch')):
                        # An unmodelled action using an int may overwrite it.
                        for param in getter.values():
                            if isinstance(param, dict):
                                name = param.get('variable') or (param.get('name') if param.get('useVariable') else None)
                                fields.pop(name, None)
                return fields
            # Carry getters across the sole FINISHED startup chain, retaining
            # the same conservative invalidation as within a state. A tested
            # transition is not an unconditional source for the next state.
            incoming_integer_fields = {}
            fields = {}
            for startup_name in startup_order:
                startup_ad = states[startup_name].get('actionData', {})
                incoming_integer_fields[startup_name] = fields
                names = startup_ad.get('actionNames', [])
                fields = preceding_integer_fields(startup_ad, len(names), fields)
                if any(action.endswith(test_kinds) for action in names):
                    fields = {}
            def integer_operand(param):
                if isinstance(param, int) and not isinstance(param, bool):
                    return None, param
                if isinstance(param, dict):
                    name = param.get('variable') or (param.get('name') if param.get('useVariable') else None)
                    if name:
                        return int_fields.get(name), None
                    value = param.get('value')
                    if isinstance(value, int) and not isinstance(value, bool):
                        return None, value
                return None, None
            starts = [states[name] for name in sorted(n for n in reachable if n in states) if any(
                a.endswith(test_kinds)
                for a in states[name].get('actionData', {}).get('actionNames', []))]
            for state in starts:
                ad = state.get('actionData', {})
                test_indices = [i for i, action in enumerate(ad.get('actionNames', []))
                                if action.endswith(test_kinds)]
                field = None
                true_event = false_event = None
                row = dict(status='unhandled', kind='PlayerDataBoolTest', field=field,
                           source=fsm_ref, owner=owner, state=state.get('name'))
                kinds = [ad['actionNames'][i].rsplit('.', 1)[-1] for i in test_indices]
                row['kind'] = '+'.join(dict.fromkeys(kinds))
                # Build event predicates in serialized action order. A later
                # gate only runs when earlier gates did not take a transition.
                gates = []
                valid = True
                uses_persistence = False
                for test_index in test_indices:
                    int_fields = preceding_integer_fields(
                        ad, test_index, incoming_integer_fields.get(state.get('name')))
                    action_kind = ad.get('actionNames', [])[test_index].rsplit('.', 1)[-1]
                    params = self._decode_params(ad, test_index)
                    if action_kind in ('IntCompare', 'IntSwitch'):
                        left, literal = integer_operand(params.get('integer1' if action_kind == 'IntCompare' else 'intVariable'))
                        right, rhs = integer_operand(params.get('integer2')) if action_kind == 'IntCompare' else (None, None)
                        reverse = False
                        if action_kind == 'IntCompare' and right and literal is not None:
                            left, rhs, reverse = right, literal, True
                        if not left or (action_kind == 'IntCompare' and rhs is None):
                            valid = False
                            row['reason'] = 'integer test is not a PlayerData getter compared with a literal'
                            break
                        field = field or left
                        if action_kind == 'IntCompare':
                            events = [(params.get('equal'), 1, rhs),
                                      (params.get('lessThan'), 5 if reverse else 3, rhs),
                                      (params.get('greaterThan'), 3 if reverse else 5, rhs)]
                        else:
                            values, events_out = params.get('compareTo'), params.get('sendEvent')
                            if not isinstance(values, list) or not isinstance(events_out, list) or len(values) != len(events_out):
                                valid = False
                                row['reason'] = 'IntSwitch arrays could not be decoded'
                                break
                            events = []
                            for value, event in zip(values, events_out):
                                _, value = integer_operand(value)
                                if value is None:
                                    valid = False
                                    break
                                events.append((event, 1, value))
                        for event, op, value in events:
                            predicate = self.node(op, fnv1a32(left), value & 0xffffffff)
                            gates.append((string_value(event) or '', '',
                                          (predicate, self.node(9, predicate, 0))))
                        continue
                    if action_kind == 'BoolTest':
                        variable = params.get('boolVariable')
                        if (not isinstance(variable, dict) or not variable.get('useVariable') or
                                variable.get('name') != 'Activated' or params.get('everyFrame') is not False):
                            valid = False
                            row['reason'] = 'BoolTest is not a one-shot persistent Activated read'
                            break
                        # The saved value is injected before startup. Any earlier
                        # action using Activated might replace it; do not guess.
                        for startup_name in reachable:
                            startup = states[startup_name].get('actionData', {})
                            limit = test_index if startup_name == state.get('name') else len(startup.get('actionNames', []))
                            for i, action in enumerate(startup.get('actionNames', [])[:limit]):
                                if action.endswith('.BoolTest'):
                                    continue
                                prior = self._decode_params(startup, i)
                                if any(isinstance(p, dict) and p.get('useVariable') and p.get('name') == 'Activated'
                                       for p in prior.values()):
                                    valid = False
                        if not valid:
                            row['reason'] = 'startup action can overwrite persistent Activated value'
                            break
                        predicate = self.node(10, fnv1a32(persistent['scene']), fnv1a32(persistent['field']))
                        predicates = (predicate, self.node(9, predicate, 0))
                        uses_persistence = True
                    elif action_kind == 'PlayerDataBoolTest':
                        field_param = params.get('boolName')
                        test_field = string_value(field_param)
                        if not test_field:
                            valid = False
                            break
                        predicates = (self.saved(test_field, True), self.saved(test_field, False))
                        field = field or test_field
                    else:
                        true_param, false_param = params.get('trueBool'), params.get('falseBool')
                        def bool_field(param):
                            return string_value(param)
                        true_field, false_field = bool_field(true_param), bool_field(false_param)
                        if not true_field or not false_field:
                            valid = False
                            break
                        predicates = (self.combine((self.saved(true_field, True),
                                                    self.saved(false_field, False))),
                                      self.disjoin((self.saved(true_field, False),
                                                    self.saved(false_field, True))))
                        field = field or true_field
                    gates.append((params.get('isTrue') or '',
                                  params.get('isFalse') or '', predicates))
                row['field'] = field
                branches = {}
                if valid and gates:
                    blocked = []
                    # Resolve FindChild results from startup states; the
                    # corresponding GO parameters are named FSM variables.
                    variables = {}
                    find_states = [states.get(startup_name, {}) for startup_name in reachable]
                    for startup in find_states:
                        startup_ad = startup.get('actionData', {})
                        for find_i, find_name in enumerate(startup_ad.get('actionNames', [])):
                            if find_name.endswith('.GetOwner'):
                                result_var = self._decode_params(startup_ad, find_i).get('storeGameObject')
                                if isinstance(result_var, dict):
                                    variables[result_var.get('name')] = owner
                            if not find_name.endswith('.FindChild'):
                                continue
                            find_params = self._decode_params(startup_ad, find_i)
                            child_name = find_params.get('childName')
                            result_var = find_params.get('storeResult')
                            if isinstance(child_name, dict):
                                child_name = child_name.get('value')
                            if isinstance(result_var, dict):
                                parent_ref = find_params.get('gameObject')
                                parent_ref = parent_ref.get('gameObject', parent_ref) if isinstance(parent_ref, dict) else {}
                                parent_value = parent_ref.get('value', {}) if isinstance(parent_ref, dict) else {}
                                parent_id = int(parent_value.get('m_PathID') or 0) if isinstance(parent_value, dict) else 0
                                parent_id = parent_id or owner
                                variables[result_var.get('name')] = child_of(parent_id, child_name)
                    valid_events = {t.get('fsmEvent', {}).get('name')
                                    for t in state.get('transitions', [])
                                    if t.get('toState') in states}
                    exhaustive = any(all(event and event in valid_events for event in gate[:2])
                                     for gate in gates)
                    event_gates = list(gates)
                    # Blank events finish the remaining actions before FINISHED.
                    if not exhaustive and 'FINISHED' in valid_events:
                        event_gates.append(('FINISHED', '',
                                            (self.constant(True), self.constant(False))))
                        exhaustive = True
                    for gate_true, gate_false, predicates in event_gates:
                        for event_name, predicate, complement in ((gate_true, predicates[0], predicates[1]),
                                                                  (gate_false, predicates[1], predicates[0])):
                            # Empty PlayMaker events let the next action run;
                            # FINISHED is sent only after the whole state ends.
                            if not event_name:
                                continue
                            transition = next((t for t in state.get('transitions', [])
                                               if t.get('fsmEvent', {}).get('name') == event_name), None)
                            branch_state = states.get(transition.get('toState')) if transition else None
                            if not branch_state:
                                continue
                            guard = self.combine([predicate, *blocked])
                            assignments = {}
                            branch_path = [branch_state]
                            while True:
                                next_state = next_static_state(branch_path[-1])
                                if not next_state or next_state in branch_path:
                                    break
                                branch_path.append(next_state)
                            branch_ad = branch_state.get('actionData', {})
                            branch_variables = dict(variables)
                            for path_state in branch_path:
                                path_ad = path_state.get('actionData', {})
                                for find_i, find_name in enumerate(path_ad.get('actionNames', [])):
                                    if not find_name.endswith('.FindChild'):
                                        continue
                                    find_params = self._decode_params(path_ad, find_i)
                                    child_name = find_params.get('childName')
                                    result_var = find_params.get('storeResult')
                                    if isinstance(child_name, dict):
                                        child_name = child_name.get('value')
                                    if isinstance(result_var, dict):
                                        branch_variables[result_var.get('name')] = child_of(owner, child_name)
                            for path_state in branch_path:
                                path_ad = path_state.get('actionData', {})
                                for action_i, action_name in enumerate(path_ad.get('actionNames', [])):
                                    action_kind = action_name.rsplit('.', 1)[-1]
                                    if action_kind == 'DestroySelf':
                                        action_params = self._decode_params(path_ad, action_i)
                                        # Destroying the owner removes its renderers, colliders
                                        # and children through their existing owner conditions.
                                        if (set(action_params) == {'detachChildren'} and
                                                action_params['detachChildren'] is False):
                                            assignments[('visual', owner)] = False
                                        continue
                                    if action_kind not in ('ActivateGameObject', 'ActivateAllChildren', 'SetCollider', 'SetSpriteRenderer', 'SetMeshRenderer'):
                                        continue
                                    action_params = self._decode_params(path_ad, action_i)
                                    target_param = action_params.get('gameObject')
                                    use_owner = isinstance(target_param, dict) and target_param.get('ownerOption') == 0
                                    target_var = target_param.get('gameObject', target_param) if isinstance(target_param, dict) else target_param
                                    target_name = target_var.get('name') if isinstance(target_var, dict) else None
                                    target_id = owner if use_owner else branch_variables.get(target_name)
                                    if not target_id and isinstance(target_var, dict):
                                        target_id = int(target_var.get('value', {}).get('m_PathID') or 0)
                                    active = action_params.get('activate' if action_kind in ('ActivateGameObject', 'ActivateAllChildren') else 'active')
                                    if target_id and isinstance(active, bool):
                                        kind = {'ActivateGameObject': 'visual', 'ActivateAllChildren': 'visual', 'SetCollider': 'collider',
                                                'SetSpriteRenderer': 'renderer', 'SetMeshRenderer': 'renderer'}[action_kind]
                                        target_ids = children.get(target_id, ()) if action_kind == 'ActivateAllChildren' else (target_id,)
                                        for ident in target_ids:
                                            assignments[(kind, ident)] = active
                            branches.setdefault(event_name, []).append((guard, assignments))
                            # A transition on this event consumes this gate
                            # before subsequent startup actions are evaluated.
                            blocked.append(complement)
                    keys = {key for outcomes in branches.values() for _, assignments in outcomes
                            for key in assignments}
                    inactive_persistence = uses_persistence and persistent.get('inactive_owner', False)
                    if inactive_persistence:
                        # A collected item can only suppress its external gate.
                        # Other writes require component/FSM execution ordering.
                        if (not self.owner_conditions.get(owner) or keys != {('visual', owner)} or
                                any(value for outcomes in branches.values() for _, assignments in outcomes
                                    for value in assignments.values())):
                            row['reason'] = persistent['reason'] = (
                                'persistent owner starts inactive; external activation or suppression is unresolved')
                            self.inventory.append(row)
                            continue
                    supported = 0
                    for kind, target_id in sorted(keys):
                        initial = True if inactive_persistence else self._initial_component_active(target_id, kind, objects)
                        outcomes = [outcome for group in branches.values() for outcome in group]
                        active_guards = [guard for guard, assignments in outcomes
                                         if assignments.get((kind, target_id), initial)]
                        if initial and not exhaustive:
                            fallback = self.combine(blocked)
                            active_guards.append(fallback if fallback is not None else self.constant(True))
                        expr = (self.disjoin(active_guards) if active_guards
                                else self.constant(False))
                        conditions = {'visual': self.owner_conditions, 'collider': self.collider_conditions,
                                      'renderer': self.renderer_conditions}[kind]
                        conditions.setdefault(target_id, []).append(expr)
                        supported += 1
                    if supported and valid:
                        row.update(status='supported', true_event=[g[0] for g in gates],
                                   false_event=[g[1] for g in gates], targets=[k[1] for k in sorted(keys)])
                        if uses_persistence:
                            persistent.update(status='supported', targets=row['targets'])
                            persistent.pop('reason', None)
                    elif valid:
                        row['reason'] = 'branch targets could not be resolved from initial ActivateGameObject actions'
                else:
                    row.setdefault('reason', 'could not decode startup PlayerData bool/int parameters')
                self.inventory.append(row)

    def add_targets(self, targets):
        """Add `(kind-name, id, owner-path, static-active)` records."""
        out = []
        seen = set()
        for kind, ident, owners, active in targets:
            node = self.condition_for_owners(owners)
            # Component toggles affect this GO only, unlike SetActive on a parent.
            component_conditions = (self.collider_conditions if kind in ('collider', 'gate') else
                                    self.renderer_conditions if kind == 'visual' else {})
            expressions = ([node] if node is not None else [])
            if owners:
                expressions += component_conditions.get(owner_id(owners[0]), [])
            node = self.combine(expressions)
            if node is None:
                continue
            key = (TARGET_KINDS[kind], int(ident), node)
            if key not in seen:
                seen.add(key)
                out.append(key)
        if len(out) > MAX_TARGETS:
            raise ValueError('Activation target capacity exceeded')
        return out

    def write(self, path, targets):
        data = bytearray(struct.pack('<4sII', b'HKA1', len(self.nodes), len(targets)))
        for node in self.nodes:
            data.extend(struct.pack('<III', *node))
        for target in targets:
            data.extend(struct.pack('<III', *target))
        Path(path).write_bytes(data)
        return data

    def manifest(self, targets):
        return dict(format='HKA1 <4sII><III nodes><III targets>',
                    node_count=len(self.nodes), target_count=len(targets),
                    nodes=[dict(index=i, op=n[0], a=n[1], b=n[2]) for i, n in enumerate(self.nodes)],
                    targets=[dict(kind=t[0], id=t[1], node=t[2]) for t in targets],
                    inventory=self.inventory,
                    limitations=['Unsupported source actions are retained in activation inventory and use serialized initial state.'])


def visual_targets(visual_source, visual_map):
    """Visual gates for every emitted visual owner.

    tk2d rows are merged from tk2d/manifest.json and are absent from
    room-visuals-map.json. Without a gate for their owner, a conditional row
    (for example Crossroads_06 infection Splat/Spit under infected_blockade)
    is drawn ungated while its progression condition is never applied.
    """
    targets = [('visual', owner_id(item['go']), item.get('owners', []), item.get('active', True))
               for item in visual_map.get('inventory', [])]
    seen = {target[1] for target in targets}
    manifest = Path(visual_source) / 'tk2d' / 'manifest.json'
    if manifest.is_file():
        for row in json.loads(manifest.read_text(encoding='utf-8')).get('inventory', []):
            owner = owner_id(row['go'])
            if owner in seen:
                raise ValueError(f'tk2d owner {owner} duplicates a room visual owner')
            seen.add(owner)
            targets.append(('visual', owner, row.get('owners', []), row.get('active', True)))
    return targets


def export_scene(terrain_source, visual_source, output, target):
    """Write per-scene HKA1 for emitted visual, terrain, gate, and spawn records."""
    terrain_source, visual_source = Path(terrain_source), Path(visual_source)
    output, target = Path(output), Path(target)
    terrain_map = json.loads((terrain_source / 'terrain-map.json').read_text(encoding='utf-8'))
    visual_map = json.loads((visual_source / 'room-visuals-map.json').read_text(encoding='utf-8'))
    compiler = ActivationCompiler([terrain_source, visual_source], [terrain_map, visual_map])
    targets = visual_targets(visual_source, visual_map)
    terrain_manifest = json.loads((output / 'terrain' / 'manifest.json').read_text(encoding='utf-8'))
    terrain_by_source = {row['collider']: row for row in terrain_map.get('inventory', [])}
    for shape in terrain_manifest.get('shapes', []):
        row = terrain_by_source.get(shape.get('source'))
        if row:
            targets.append(('collider', shape['id'], row.get('owners', []), True))
    scene_manifest_path = output / 'manifest.json'
    scene_manifest = json.loads(scene_manifest_path.read_text(encoding='utf-8'))
    for index, gate in enumerate(scene_manifest.get('gates', [])):
        owners = gate.get('owners', [])
        if owners:
            targets.append(('gate', index, owners, gate.get('active', True)))
    for spawn in scene_manifest.get('enemy_spawns', []):
        owners = spawn.get('owners', [])
        if owners:
            targets.append(('spawn', spawn['owner'], owners, spawn.get('active', True)))
    # One-based indices preserve the unchanged acid.bin layout and identify
    # the same authored regions used by RoomFlow/supportWater.
    for index, hazard in enumerate(scene_manifest.get('acid_hazards', []), 1):
        targets.append(('hazard', index, hazard.get('owners', []), True))
    for index, owners in enumerate(scene_manifest.get('acid_water_owners', []), 1):
        targets.append(('acid_water', index, owners, True))
    compiled = compiler.add_targets(targets)
    output.mkdir(parents=True, exist_ok=True); target.mkdir(parents=True, exist_ok=True)
    data = compiler.write(output / 'activation.bin', compiled)
    shutil.copyfile(output / 'activation.bin', target / 'activation.bin')
    audit = compiler.manifest(compiled)
    audit['binary_sha256'] = hashlib.sha256(data).hexdigest()
    (output / 'activation.json').write_text(json.dumps(audit, indent=2) + '\n', encoding='utf-8')
    scene_manifest['activation'] = dict(format='HKA1', nodes=len(compiler.nodes), targets=len(compiled),
                                        binary='activation.bin', binary_sha256=audit['binary_sha256'],
                                        inventory='activation.json')
    scene_manifest_path.write_text(json.dumps(scene_manifest, indent=2) + '\n', encoding='utf-8')
    return audit
