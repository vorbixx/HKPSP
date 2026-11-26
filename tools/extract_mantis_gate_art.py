"""Recover source-confirmed Mantis lever/gate artwork for Fungus2_14/15.

Run through validation-lock.ps1 -Exclusive. Shared original exports stay read
only; each room is copied into this worktree's private obstacle staging area.
"""
from functools import partial
from pathlib import Path
import shutil

from assetripper_client import ROOT
from extract_room_tk2d import extract
from extract_world_switch_session import run


def recover(client, source, output, *, owners):
    if output.is_symlink() or output.resolve() != output.absolute():
        raise ValueError('Mantis gate staging must be a real worktree directory')
    shutil.copytree(source.parent, output, dirs_exist_ok=True)
    extract(client, output / 'visuals', only_owners=owners)


def recover_room(scene, level, owners, port, output_name):
    run(port, ROOT / '_private/minimum-obstacles' / output_name,
        scene=scene, level=level,
        extractor=partial(recover, owners=set(owners)))


if __name__ == '__main__':
    recover_room('Fungus2_14', 178,
                 {'level178-434.json', 'level178-2415.json'}, 58411,
                 'mantis-14-room')
    recover_room('Fungus2_15', 179,
                 {'level179-247.json', 'level179-2722.json'}, 58412,
                 'mantis-15-room')
