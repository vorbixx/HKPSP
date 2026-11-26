"""Stage one immutable room export and recover its original gate artwork.

Run through validation-lock.ps1 -Exclusive. The shared saved room and original
install copy are read only; output is limited to the recovered obstacle export.
"""
import shutil
import argparse
from functools import partial

from assetripper_client import ROOT
from extract_room_tk2d import extract
from extract_world_switch_session import run


def recover(client, source, output, *, owners=None):
    if output.is_symlink() or output.parent.resolve() != (ROOT / '_private/minimum-obstacles').resolve():
        raise ValueError('Ruins gate staging must stay in the recovered obstacle directory')
    shutil.copytree(source.parent, output, dirs_exist_ok=True)
    extract(client, output / 'visuals',
            only_owners=owners or {'level93-539.json', 'level93-559.json'})


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scene', choices=('Ruins1_05', 'Ruins1_05b', 'Ruins1_05c'), default='Ruins1_05b')
    args = parser.parse_args()
    upper = args.scene == 'Ruins1_05c'
    owners = {'level94-1555.json', 'level94-1385.json', 'level94-1493.json', 'level94-1258.json'} if upper else None
    room, level = ('ruins-gate-05c-room', 94) if upper else ('ruins-gate-room', 93)
    if args.scene == 'Ruins1_05':
        room, level = 'ruins-gate-05-room', 92
        owners = {'level92-631.json', 'level92-382.json'}
    run(58409, ROOT / '_private/minimum-obstacles' / room,
        scene=args.scene, level=level, extractor=partial(recover, owners=owners))
