"""Source-backed initial tk2d artwork sharing the room sprite/owner pipeline."""
import hashlib
import json
from pathlib import Path

def runtime_owned(components, load=None):
    # These classes own destruction, persistence or hazard state. Their visuals
    # must come from the matching runtime actor rather than static room art.
    if any(k.startswith('NPC') or k in {
        'HealthManager', 'DialogueBox', 'ShopMenuStock', 'DamageHero',
        'EnemyDeathEffects', 'JellyEgg', 'GeoRock', 'Breakable',
        'PersistentBoolItem', 'PersistentIntItem',
    } for k in components):
        return True
    for ref in components.get('PlayMakerFSM', ()):
        if load is None:
            raise ValueError('FSM ownership requires source data')
        fsm = load(ref).get('m_Structure', {}).get('fsm', {})
        # These FSMs own NPC lifecycle or runtime mesh visibility, which the
        # static artwork activation compiler cannot preserve.
        if fsm.get('name', '').casefold() in {
                'npc_control', 'conversation control', 'mantis gate'}:
            return True
        actions = {action.rsplit('.', 1)[-1]
                   for state in fsm.get('states', ())
                   for action in state.get('actionData', {}).get('actionNames', ())}
        if actions & {'DestroySelf', 'SetMeshRenderer'}:
            return True
    return False



def merge(mapping, source):
    path = source / 'tk2d/manifest.json'
    if not path.is_file():
        return []
    supplement = json.loads(path.read_text())
    anchor = hashlib.sha256((source / 'room-visuals-map.json').read_bytes()).hexdigest()
    if (supplement['build'], supplement['scene'], supplement['room_manifest_sha256']) != (
            mapping['build'], mapping['scene'], anchor):
        raise ValueError('Changed tk2d room anchors')
    def load(ref):
        return json.loads((source / ref).read_bytes())
    from scene_levers import controllers
    declared = {(a['owner'],a['gate']) for a in supplement.get('levers', [])}
    pairs = [p for p in controllers(mapping, load) if (p['lever']['go'],p['gate']['go']) in declared]
    if len(pairs) != len(declared):
        raise ValueError('Changed source lever controller')
    controlled = {p[k]['go'] for p in pairs for k in ('lever','gate')}
    for row in supplement['inventory']:
        hierarchy = [supplement['owner_components'].get(owner, {})
                     for owner in row['owners'] if owner not in controlled]
        if (row['go'] not in controlled and runtime_owned(row['components'], load)) or any(runtime_owned(c, load) for c in hierarchy):
            raise ValueError('Runtime-owned tk2d mesh; re-extract the supplement: ' + row['_tk2d_mesh'])
    for name, sha in supplement['source_sha256'].items():
        if Path(name).is_absolute() or '..' in Path(name).parts:
            raise ValueError('Invalid tk2d dependency path')
        mapping['source_sha256']['tk2d/' + name] = sha
    mapping['inventory'].extend(supplement['inventory'])
    mapping['sprites'].update(supplement['sprites'])
    mapping.setdefault('owner_components', {}).update(supplement['owner_components'])
    return supplement['inventory']


def decode(atlas, sprite):
    from convert_hero import decode_sprite
    return decode_sprite(atlas, sprite['_tk2d_definition'])
