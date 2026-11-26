"""Source-backed SceneManager.OnCameraAspectChanged border placement."""
import hashlib
import json
import math
from pathlib import Path
from scene_camera import authored_camera_limits


def border_geometry(width, height, thickness, mask, depth, layer, order, aspect=400/240):
    if (any(not math.isfinite(v) or not 0 < v <= 100000 for v in (width, height, thickness, aspect))
            or not math.isfinite(depth) or abs(depth) > 100000
            or type(mask) is not int or mask not in (-1, *range(16))):
        raise ValueError('Invalid SceneBorder dimensions or mask')
    delta = aspect - 1.7777778
    horizontal = .5 if delta > .01 else 0
    vertical = .5 if delta < -.01 else 0
    x = thickness
    # Source bit order: up, down, left, right. Keep its asymmetric bottom X shift.
    rectangles = [(0, [-x, height-vertical, width+x, height+x-vertical]),
                  (1, [-x+vertical, -x, width+x+vertical, 0]),
                  (2, [-x+horizontal, -x, horizontal, height+x]),
                  (3, [width-horizontal, -x, width+x-horizontal, height+x])]
    return [dict(bounds=[a, b, depth, c, d, depth], layer=layer, order=order)
            for bit, (a, b, c, d) in rectangles if mask & (1 << bit)]


def authored_scene_borders(source, scene):
    source = Path(source)
    path = source/'borders/scene-borders-map.json'
    if not path.exists():
        # Legacy scene exports may omit borders, but a saved bundle must be complete.
        if path.parent.is_dir():
            raise ValueError('Incomplete SceneBorder source export')
        manager = source/'scene-manager.json'
        if manager.is_file():
            settings = json.loads(manager.read_text())
            if (settings.get('sceneBordersMask', 0) != 0 and
                    settings.get('borderPrefab', {}).get('m_PathID', 0)):
                raise ValueError('Missing SceneBorder source for ' + scene +
                                 '; run extract_scene_borders.py --scene ' + scene)
        return []
    mapping = json.loads(path.read_text())
    if mapping.get('build') != 22529139 or mapping.get('scene') != scene:
        raise ValueError('SceneBorder source build/scene mismatch')
    raw = (source/'scene-manager.json').read_bytes()
    if hashlib.sha256(raw).hexdigest() != mapping['scene_manager_sha256']:
        raise ValueError('Changed SceneBorder manager source')
    settings = json.loads(raw)
    if settings['sceneBordersMask'] != mapping['mask']:
        raise ValueError('Changed SceneBorder mask')
    cache = {}
    for name, sha in mapping['source_sha256'].items():
        if Path(name).name != name or not name.endswith('.json'):
            raise ValueError('Invalid SceneBorder source filename')
        raw = (path.parent/name).read_bytes()
        if hashlib.sha256(raw).hexdigest() != sha:
            raise ValueError('Changed SceneBorder source: ' + name)
        cache[name] = json.loads(raw)
    go, transform, renderer, sprite, shader = [cache[mapping[k]]
        for k in ('prefab', 'transform', 'renderer', 'sprite', 'shader')]
    rect, pivot, ppu = sprite['m_Rect'], sprite['m_Pivot'], sprite['m_PixelsToUnits']
    if (go['m_Name'] != 'SceneBorder' or not go['m_IsActive'] or not renderer['m_Enabled']
            or sprite['m_Name'] != 'level_tile' or ppu <= 0
            or rect['m_Width']/ppu != 1 or rect['m_Height']/ppu != 1
            or pivot != dict(m_X=.5, m_Y=.5)
            or renderer['m_Color'] != dict(m_R=0, m_G=0, m_B=0, m_A=1)
            or renderer['m_SortingLayer'] != 10
            or shader['m_ParsedForm']['m_Name'] != 'Sprites/Darkness Sprite'):
        raise ValueError('Unsupported SceneBorder prefab')
    limits = authored_camera_limits(source, scene)
    if limits is None:
        raise ValueError('SceneBorder requires source TileMap dimensions')
    return border_geometry(limits[2]+14.6, limits[3]+8.3,
                           transform['m_LocalScale']['m_X'], mapping['mask'],
                           transform['m_LocalPosition']['m_Z'], renderer['m_SortingLayer'],
                           renderer['m_SortingOrder'])
