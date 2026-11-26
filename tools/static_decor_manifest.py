"""Fail-closed static-owner classification from an exported scene hierarchy."""
import hashlib
import json
from pathlib import Path

from scene_activation import ActivationCompiler


def passive_component(data):
    if 'm_Script' in data or 'm_Animator' in data or 'm_Controller' in data:
        return False
    return (('m_Father' in data and 'm_LocalPosition' in data) or
            ('m_Materials' in data and 'm_SortingOrder' in data) or
            ('m_Mesh' in data and 'm_GameObject' in data) or
            ('m_IsTrigger' in data and 'm_Offset' in data))


def classify(source, bank_owners, protected=()):
    source = Path(source)
    mapping = json.loads((source / 'room-visuals-map.json').read_bytes())
    collections = {row['go'].rsplit('-', 1)[0]
                   for row in mapping.get('inventory', []) if row.get('go')}
    collection = next(iter(collections)) if len(collections) == 1 else None
    sources, maps = [source], [mapping]
    terrain = source.parent / 'terrain'
    if (terrain / 'terrain-map.json').exists():
        sources.append(terrain)
        maps.append(json.loads((terrain / 'terrain-map.json').read_bytes()))
    activation = ActivationCompiler(sources, maps)
    cache, hashes = {}, {}

    def read(ref):
        if ref not in cache:
            path = source / ref
            # Missing exports fail closed without opening undeclared inputs.
            if not path.is_file():
                raise FileNotFoundError(f'missing owner export: {ref}')
            raw = path.read_bytes()
            hashes[ref] = hashlib.sha256(raw).hexdigest()
            cache[ref] = json.loads(raw)
        return cache[ref]

    admitted, rejected, evidence = [], {}, {}
    for owner in sorted(bank_owners):
        if owner in protected:
            rejected[owner] = 'runtime animation/state binding'
            continue
        ref, chain, visited = f'{collection}-{owner}.json', [], set()
        try:
            if collection is None or not collection.startswith('level') or not collection[5:].isdigit():
                raise ValueError('missing or ambiguous scene collection')
            while ref:
                if ref in visited:
                    raise ValueError('cyclic hierarchy')
                visited.add(ref)
                obj = read(ref)
                if not obj.get('m_IsActive', False):
                    raise ValueError('inactive hierarchy')
                chain.append(ref)
                transform = None
                for component in obj['m_Components']:
                    pointer = component['m_Component']
                    if pointer['m_FileID'] != 0:
                        raise ValueError('external component')
                    data = read(f"{collection}-{pointer['m_PathID']}.json")
                    if not passive_component(data):
                        raise ValueError('animator/FSM/script/unknown component')
                    if 'm_Father' in data:
                        transform = data
                if transform is None:
                    raise ValueError('missing transform')
                father = transform['m_Father']
                if father['m_FileID'] != 0:
                    raise ValueError('external parent')
                if father['m_PathID']:
                    parent = read(f"{collection}-{father['m_PathID']}.json")['m_GameObject']
                    if parent['m_FileID'] != 0:
                        raise ValueError('external parent object')
                    ref = f"{collection}-{parent['m_PathID']}.json"
                else:
                    ref = None
            if activation.condition_for_owners(chain) is not None:
                raise ValueError('activation predicate')
        except (OSError, KeyError, ValueError) as error:
            rejected[owner] = str(error)
            continue
        admitted.append(owner)
        evidence[owner] = chain
    return dict(schema=1, scene=mapping.get('scene'), owners=admitted, rejected=rejected,
                chains=evidence, source_sha256=hashes)
