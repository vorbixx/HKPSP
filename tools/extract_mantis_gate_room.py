"""Recover the source-confirmed Fungus2_18 Mantis gate in an isolated room.

Run under validation-lock.ps1 -Exclusive; shared original exports stay read only.
Existing non-actor room artwork is retained by the targeted tk2d extraction.
"""
from functools import partial
from assetripper_client import ROOT
from extract_ruins_gate_art import recover
from extract_world_switch_session import run


if __name__ == '__main__':
    run(58409, ROOT / '_private/minimum-obstacles/mantis-18-room',
        scene='Fungus2_18', level=183,
        extractor=partial(recover, owners={'level183-1248.json', 'level183-3194.json'}))
