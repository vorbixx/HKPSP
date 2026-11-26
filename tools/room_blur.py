"""Source BlurPlane boundaries shared by visual export and runtime banks."""
import math
import warnings

LEGACY_SPLIT_DEPTH = 13.71
# Source LightBlurredBackground camera far=1000, camera world z=-38.1.
NO_PLANE_SPLIT_DEPTH = 961.9


def extract_blur_planes(export, collection, rows=None):
    rows = export.client.search('BlurPlane') if rows is None else rows
    planes = []
    for row in rows:
        if row['collection'] != collection or row['type'] != 'GameObject':
            continue
        c = row['path']['C']
        parts = export.components(c, row['path']['D'])
        for component in parts.get('BlurPlane', []):
            if not export.cache[component].get('m_Enabled', 1):
                continue
            chain, owners, active = [], [], True
            key = parts['Transform'][0]
            while True:
                if key in chain:
                    raise ValueError('Cyclic BlurPlane owner chain')
                transform = export.cache[key]
                chain.append(key)
                owner, owner_key, _ = export.resolve(c, transform['m_GameObject'])
                owners.append(owner_key)
                active = active and bool(owner['m_IsActive'])
                if not transform['m_Father']['m_PathID']:
                    break
                _, key, c = export.resolve(c, transform['m_Father'])
            if active:
                planes.append(dict(component=component, chain=chain, owners=owners))
    return planes


def audited_blur_planes(mapping, read):
    """Recover boundaries already saved by pre-blur-plane room exports."""
    rows = [row for row in mapping.get('mesh_renderer_audit', [])
            if row.get('components', {}).get('BlurPlane')]
    if not rows:
        return None
    planes = []
    for row in rows:
        parts = row['components']
        if not any(read(key).get('m_Enabled', 1) for key in parts['BlurPlane']):
            continue
        key = parts['Transform'][0]
        chain, active = [], True
        while True:
            if key in chain or len(chain) >= 64:
                raise ValueError('Cyclic BlurPlane owner chain')
            transform = read(key)
            chain.append(key)
            collection = key.rsplit('-', 1)[0]
            def local(ref):
                if ref['m_FileID'] != 0:
                    raise ValueError('External BlurPlane owner reference')
                return f"{collection}-{ref['m_PathID']}.json"
            active = active and bool(read(local(transform['m_GameObject']))['m_IsActive'])
            if not transform['m_Father']['m_PathID']:
                break
            key = local(transform['m_Father'])
        if active:
            planes.append(dict(chain=chain))
    return planes


def split_depth(mapping, read, project):
    planes = mapping.get('blur_planes')
    if planes is None:
        try:
            planes = audited_blur_planes(mapping, read)
        except FileNotFoundError as error:
            # Some old exports did not save the BlurPlane's parent. Retain
            # compatibility, but make the missing source visible to regeneration.
            warnings.warn(f"{mapping.get('scene', 'room')}: incomplete BlurPlane export: {error}")
            planes = None
        if planes is None:
            return LEGACY_SPLIT_DEPTH
    depths = [project((0, 0, 0), [read(k) for k in plane['chain']])[2]
              for plane in planes]
    # BlurPlane.ClosestBlurPlane returns the first enabled plane. All ten
    # sampled rooms have one; preserve export order for multiple planes.
    depth = depths[0] + .01 if depths else NO_PLANE_SPLIT_DEPTH
    if not math.isfinite(depth) or not -28.1 <= depth <= NO_PLANE_SPLIT_DEPTH:
        raise ValueError('Invalid source BlurPlane boundary')
    return depth
