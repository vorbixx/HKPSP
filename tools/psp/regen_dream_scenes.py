"""Rebuild locally sourced dream rooms from the original game data."""

import socket
import sys
from functools import partial
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import room_packages

room = room_packages.room
sys.path.insert(0, str(room.REPO / 'tools'))
room.import_pil()

import convert_scenes
import extract_scenes
import extract_world_switch_session
from scene_sources import saved_scenes


def export(scene, level, client, unused, output):
    extract_scenes.extract(client, scene, level, output)


def main():
    source = room.source_root().resolve()
    sys.path.append(str(source / '_private/tools/audio-python'))
    assets = room.REPO / '_private/converted-psp'
    exports = assets / 'route-scenes-source'
    host = assets / 'dream-host'
    catalog = tuple(dict.fromkeys((*convert_scenes.SCENES, *room_packages.DREAM_SCENES,
                                   *saved_scenes(source / '_private/research/crossroads-scenes'))))
    for scene, level in room_packages.DREAM_SCENES.items():
        if not extract_scenes.already_exported(exports / scene):
            with socket.socket() as listener:
                listener.bind(('127.0.0.1', 0))
                port = listener.getsockname()[1]
            extract_world_switch_session.run(port, exports, scene=scene, level=level,
                                             extractor=partial(export, scene, level), input_root=source)
        convert_scenes.convert(scene, host / 'romfs', catalog, output_root=host / 'scenes',
                               source_root=exports / scene, frozen_catalog=True)
    room_packages.convert(source, assets, room_packages.DEFAULT_STAGE, rooms=room_packages.DREAM_SCENES)


if __name__ == '__main__':
    main()
