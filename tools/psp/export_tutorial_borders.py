"""Export the King's Pass SceneManager, border prefab and camera TileMap.

Writes `_private/research/tutorial-scene/Tutorial_01`. convert_room_visuals.py
places the runtime SceneBorders from this source with the SceneManager
formula when no original runtime capture was imported. Load Tutorial_01
(level6) into AssetRipper first.
"""
import argparse
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from assetripper_client import Client
from extract_crawler import Export
from extract_scene_borders import extract, valid_export
from extract_scenes import valid_scene_manager

OUTPUT = ROOT / '_private/research/tutorial-scene/Tutorial_01'
LEVEL = 6


def scene_manager(client, base):
    e = Export(client, base)
    for manager in client.search('SceneManager'):
        if manager['collection'] != f'level{LEVEL}' or manager['type'] != 'GameObject':
            continue
        parts = e.components(manager['path']['C'], manager['path']['D'])
        if parts.get('SceneManager'):
            settings = e.cache[parts['SceneManager'][0]]['m_Structure']
            if valid_scene_manager(settings):
                return settings
    raise ValueError('Tutorial_01 has no valid SceneManager')


def export(client, base=OUTPUT):
    base.mkdir(parents=True, exist_ok=True)
    manager = base / 'scene-manager.json'
    if not manager.is_file():
        manager.write_text(json.dumps(scene_manager(client, base)))
    if not valid_export(base, base.name):
        extract(client, base, LEVEL)
    if not valid_export(base, base.name):
        raise ValueError('Incomplete Tutorial_01 SceneBorder export')
    print('Tutorial_01 borders:', base)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=58291)
    export(Client(parser.parse_args().port))
