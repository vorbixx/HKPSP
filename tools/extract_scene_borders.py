"""Save the source prefab used by SceneManager's runtime room borders.

Run through validation-lock.ps1 -Exclusive with the scene loaded in AssetRipper.
"""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
from assetripper_client import Client, ROOT
from extract_crawler import Export
from extract_scene_camera import extract as camera


def source_scene_index(base):
    """Resolve any saved room, including rooms outside the legacy scene list."""
    base = Path(base)
    mapping = json.loads((base/'visuals/room-visuals-map.json').read_text())
    if (mapping.get('build') != 22529139 or mapping.get('scene') != base.name
            or not re.fullmatch(r'[A-Za-z0-9_]{1,31}', base.name)):
        raise ValueError('SceneBorder visual source build/scene mismatch')
    indices = set()
    for row in mapping['inventory']:
        match = re.fullmatch(r'level(\d+)-\d+\.json', row['renderer'])
        if match is None:
            raise ValueError('Invalid SceneBorder visual collection')
        indices.add(int(match[1]))
    if len(indices) != 1:
        raise ValueError('SceneBorder requires one source scene collection')
    return indices.pop()


def valid_export(base, scene):
    """Return whether a saved border prefab export matches its SceneManager."""
    path = base/'borders/scene-borders-map.json'
    try:
        manifest = json.loads(path.read_text(encoding='utf-8'))
        manager = base/'scene-manager.json'
        if (manifest.get('build') != 22529139 or manifest.get('scene') != scene
                or not isinstance(manifest.get('source_sha256'), dict)
                or any(key not in manifest for key in
                       ('mask', 'prefab', 'transform', 'renderer', 'sprite',
                        'material', 'shader'))
                or manifest.get('scene_manager_sha256') !=
                hashlib.sha256(manager.read_bytes()).hexdigest()):
            return False
    except (OSError, ValueError, TypeError, KeyError):
        return False
    try:
        with os.scandir(base/'borders') as entries:
            present = {entry.name for entry in entries if entry.is_file()}
    except OSError:
        return False
    return set(manifest['source_sha256']) <= present


def extract_prefab(client, base, index):
    scene = base.name
    raw = (base/'scene-manager.json').read_bytes()
    settings = json.loads(raw)
    rows = [r for r in client.search('_SceneManager')
            if r['collection'] == f'level{index}' and r['type'] == 'GameObject'
            and r['name'] == '_SceneManager']
    if len(rows) != 1:
        raise ValueError('Missing or ambiguous source SceneManager')
    output = base/'borders'
    output.mkdir(exist_ok=True)
    export = Export(client, output)
    prefab, key, collection = export.resolve(rows[0]['path']['C'], settings['borderPrefab'])
    parts = export.components(collection, settings['borderPrefab']['m_PathID'])
    if len(parts.get('SpriteRenderer', [])) != 1 or len(parts.get('Transform', [])) != 1:
        raise ValueError('Unsupported SceneBorder prefab hierarchy')
    renderer = export.cache[parts['SpriteRenderer'][0]]
    if len(renderer['m_Materials']) != 1:
        raise ValueError('Unsupported SceneBorder materials')
    _, sprite, _ = export.resolve(collection, renderer['m_Sprite'])
    material, material_key, mc = export.resolve(collection, renderer['m_Materials'][0])
    _, shader, _ = export.resolve(mc, material['m_Shader'])
    manifest = dict(build=22529139, scene=scene, mask=settings['sceneBordersMask'],
                    scene_manager_sha256=hashlib.sha256(raw).hexdigest(), prefab=key,
                    transform=parts['Transform'][0], renderer=parts['SpriteRenderer'][0],
                    sprite=sprite, material=material_key, shader=shader, source_sha256=export.hashes)
    (output/'scene-borders-map.json').write_text(json.dumps(manifest, indent=2))


def extract(client, base, index):
    extract_prefab(client, base, index)
    camera(client, base/'camera', f'level{index}', base.name)
    print(base.name, 'exported runtime border prefab')


def extract_converted(client, report):
    """Backfill border inputs for the rooms that are actually installed."""
    from eastern_rooms import scene_source
    from scene_borders import authored_scene_borders

    rows = []
    for path in sorted((ROOT/'_private/converted/scenes').glob('*/art/manifest.json')):
        scene = path.parent.parent.name
        base = (ROOT/scene_source(scene)).resolve()
        manager = base/'scene-manager.json'
        art = json.loads(path.read_text())
        row = dict(scene=scene, before=len(art.get('runtime_borders', [])), exported=False)
        rows.append(row)
        if not manager.is_file():
            row['skip'] = 'No generic SceneManager source'
            continue
        settings = json.loads(manager.read_text())
        if not settings.get('sceneBordersMask', 0) or not settings.get('borderPrefab', {}).get('m_PathID', 0):
            row['expected'] = 0
            continue
        if not valid_export(base, scene) or not (base/'camera/camera-map.json').is_file():
            extract(client, base, source_scene_index(base))
            row['exported'] = True
        row['expected'] = len(authored_scene_borders(base, scene))
    result = dict(rooms=len(rows), empty_before=sum(r['before'] == 0 for r in rows),
                  exported=sum(r['exported'] for r in rows), scenes=rows,
                  regenerate=[r['scene'] for r in rows if r['exported'] or
                              ('expected' in r and r['before'] != r['expected'])])
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(dict(rooms=result['rooms'], empty_before=result['empty_before'],
                          exported=result['exported'], regenerate_rooms=len(result['regenerate']))))


class CachedClient(Client):
    """A batch keeps one loaded AssetRipper session; searches/assets are immutable."""
    def __init__(self, port):
        super().__init__(port)
        self.responses = {}

    def request(self, endpoint, **query):
        key = (endpoint, tuple(sorted(query.items())))
        if key not in self.responses:
            self.responses[key] = super().request(endpoint, **query)
        return self.responses[key]


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    selection = p.add_mutually_exclusive_group(required=True)
    selection.add_argument('--scene', help='Name of a saved room export')
    selection.add_argument('--converted', action='store_true', help='Backfill all installed room border sources')
    p.add_argument('--report', type=Path, default=ROOT/'build/scene-border-backfill.json')
    p.add_argument('--port', type=int, default=58291)
    args = p.parse_args()
    if args.converted:
        extract_converted(CachedClient(args.port), args.report)
        raise SystemExit(0)
    if not re.fullmatch(r'[A-Za-z0-9_]{1,31}', args.scene):
        p.error('Invalid scene name')
    base = (ROOT/'_private/research/crossroads-scenes'/args.scene).resolve()
    extract(Client(args.port), base, source_scene_index(base))
