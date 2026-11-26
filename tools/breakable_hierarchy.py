"""Retain authored breakable hierarchies, including non-rendering roots."""


def hierarchy(export, collection, root, limit=8192):
    """Retain authored poses; prefab spawning needs a separate instance pose."""
    rows, visited = [], set()

    def visit(current_collection, asset_id, ancestry):
        go, key = export.fetch(current_collection, asset_id)
        if key in ancestry:
            raise ValueError('Cyclic breakable hierarchy: ' + key)
        if key in visited:
            return
        visited.add(key)
        if len(visited) > limit:
            raise ValueError('Breakable hierarchy export budget exceeded')
        parts = export.components(current_collection, asset_id)
        chain, owners = [], []
        transform_key, tc = parts['Transform'][0], current_collection
        while True:
            if transform_key in chain:
                raise ValueError('Cyclic breakable transform: ' + transform_key)
            chain.append(transform_key)
            transform = export.cache[transform_key]
            parent, owner, _ = export.resolve(tc, transform['m_GameObject'])
            owners.append(owner)
            if not transform['m_Father']['m_PathID']:
                break
            _, transform_key, tc = export.resolve(tc, transform['m_Father'])
        rows.append(dict(go=key, collection=current_collection, components=parts, chain=chain, owners=owners))
        transform = export.cache[parts['Transform'][0]]
        for child in transform.get('m_Children', []):
            child_transform, _, child_collection = export.resolve(current_collection, child)
            _, _, child_collection = export.resolve(child_collection, child_transform['m_GameObject'])
            visit(child_collection, child_transform['m_GameObject']['m_PathID'], ancestry | {key})

    visit(collection, root, set())
    return rows


def scene_objects(mapping):
    """Keep shared templates out of the room's controller/renderer identity space."""
    external = {ref['root'] for ref in mapping.get('breakable_references', [])
                if ref.get('scene_instance', True) is False}
    for row in mapping.get('breakable_objects', []):
        if (row.get('scene_instance', True) is not False and row['go'] not in external
                and not external.intersection(row.get('owners', []))):
            yield row
