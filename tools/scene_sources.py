"""Discover complete standalone room exports without contacting AssetRipper."""
import json
import re
from pathlib import Path

REQUIRED = ('scene-manager.json', 'terrain/terrain-map.json', 'visuals/room-visuals-map.json')


def saved_scenes(root):
    root = Path(root)
    if not root.is_dir():
        return ()
    return tuple(sorted(p.name for p in root.iterdir() if p.is_dir()
                        and re.fullmatch(r'[A-Za-z0-9_]{1,31}', p.name)
                        # GameManager loads _boss scenes additively inside their parent.
                        and not p.name.endswith('_boss')
                        and all((p / name).is_file() for name in REQUIRED)))


def gate_catalog(source, scenes):
    """Return catalog entries whose spelling can affect this room's gates."""
    terrain = Path(source) / 'terrain'
    try:
        inventory = json.loads((terrain / 'terrain-map.json').read_text(encoding='utf-8'))['inventory']
    except FileNotFoundError:
        return ()
    destinations = set()
    for row in inventory:
        components = row['components'][0]
        if 'TransitionPoint' not in components:
            continue
        transition = json.loads((terrain / components['TransitionPoint'][0]).read_text(encoding='utf-8'))
        destination = transition['m_Structure']['targetScene']
        for name in components.get('PlayMakerFSM', []):
            component = json.loads((terrain / name).read_text(encoding='utf-8'))
            # Ignore disabled Door Control components.  Their serialized
            # strings can be stale and must not make a blank TransitionPoint
            # appear to have a reachable destination.
            if component.get('m_Enabled', 1) == 0:
                continue
            fsm = component['m_Structure']['fsm']
            if fsm.get('name') != 'Door Control':
                continue
            variables = {item['name']: item['value'] for item in fsm['variables']['stringVariables']}
            if variables.get('New Scene') and variables.get('Entry Gate'):
                destination = variables['New Scene']
                break
        if destination:
            destinations.add(destination.casefold())
    return tuple(scene for scene in scenes if scene.casefold() in destinations)
