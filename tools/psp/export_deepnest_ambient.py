"""Export the Deepnest ambient particle banks (MapZones 9, 10 and 25).

Writes `_private/research/deepnest-ambient-particles`, read by
`tools/convert_deepnest_ambient.py`. Each bank is the camera particle prefab
the SceneParticlesController selects for that MapZone, walked the same way as
`tools/extract_ambient_particles.py`. Load Tutorial_01 (level6) into
AssetRipper first; the camera and controller live in resources.assets.
"""
import argparse
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from assetripper_client import Client
from extract_ambient_particles import MANIFEST, observe
from extract_decor_effects import Store, require

ZONES = (9, 10, 25)


def export(client, output):
    output.mkdir(parents=True, exist_ok=True)
    store = Store(output, client)
    banks = []
    for zone in ZONES:
        derived = observe(store, zone)
        require(not derived['used_default'], f'MapZone {zone} has no camera particle prefab')
        emitters = []
        for emitter in derived['emitters']:
            system = store.get(emitter['system'])
            emitters.append(dict(name=emitter['name'], object=emitter['object'], system=emitter['system'],
                                 renderer=emitter['renderer'],
                                 transforms=list(reversed(emitter['transforms'])),  # root to emitter
                                 material=emitter['material'], shader=emitter['shader'],
                                 texture=emitter['texture'],
                                 capacity=system['m_InitialModule']['m_MaxNumParticles'],
                                 shape=system['m_ShapeModule']['m_Type']))
        banks.append(dict(zone=zone, root=derived['root'], emitters=emitters))
    report = dict(build=22529139, banks=banks, source_sha256=store.hashes,
                  dependency_tables=store.tables, media=store.media)
    (output / MANIFEST).write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(f'Deepnest ambient: {len(banks)} banks, {sum(len(b["emitters"]) for b in banks)} emitters')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--port', type=int, default=58291)
    parser.add_argument('--output', type=Path, default=ROOT / '_private/research/deepnest-ambient-particles')
    args = parser.parse_args()
    export(Client(args.port), args.output)


if __name__ == '__main__':
    main()
