"""Convert the copied original Dung Defender and dung-ball animation bank."""
import argparse
from pathlib import Path

from assetripper_client import ROOT
from convert_crossroads_bosses import convert
from tool_paths import TEX3DS as TEX3DS_EXE


ALIASES = {'Dung': ('sharedassets354.assets', 53, {
    'Idle': 'Idle', 'Intro': 'Roar', 'Aim': 'Throw Antic 1',
    'Throw': 'Throw 1', 'Throw2': 'Throw 2', 'Dig': 'Dive In 1',
    'Erupt': 'Erupt Up', 'Fall': 'Erupt Down', 'Roll': 'Roll',
    'Recover': 'Roll End', 'Rage': 'Roar', 'Death': 'Death Fly',
    'DolphinAntic': 'Dolphin Antic', 'Dolphin': 'Dolphin Dive',
    'DolphinRecover': 'Antic R',
    'Evade': 'Evade', 'EvadeBall': 'Evade Ball', 'StunAir': 'Stun Air', 'Stun': 'Stun', 'StunRecover': 'Stun Recover',
    'Ball': 'Ball Idle', 'BallBurst': 'Ball Burst', 'Burrow': 'Burrow Effect',
})}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / '_private/original/Hollow Knight/hollow_knight_Data')
    parser.add_argument('--snapshot', type=Path)
    parser.add_argument('--save-snapshot', type=Path)
    parser.add_argument('--output', type=Path, default=ROOT / '_private/converted/dung-defender')
    parser.add_argument('--packages', type=Path, default=ROOT / '_private/tools/boss-python')
    parser.add_argument('--tex3ds', type=Path, default=Path(TEX3DS_EXE))
    parser.add_argument('--scale', type=float, default=.5)
    args = parser.parse_args()
    args.false_knight_only = False
    convert(args, aliases=ALIASES, stem='dung', component_scenes=(), complete_library=False)


if __name__ == '__main__':
    main()
