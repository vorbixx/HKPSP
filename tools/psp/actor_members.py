"""Generate Tutorial_01's membership from the resident 3DS load block.

Unknown tables fail closed. The source line, runtime binding and evidence are
reported, including explicit exclusions; this is not a hand-maintained member list.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
ATLASES = {
    'hero.bin': ('hero', 'appendHero'),
    'crawler-anim.bin': ('crawler', 'appendCrawlers'),
    'buzzer-anim.bin': ('buzzer', 'appendBuzzers'),
    'shade-anim.bin': ('shade', 'appendShade'),
    'geo-visual.bin': ('geo', 'appendGeo'),
    'great-door-visual.bin': ('great-door', 'appendDoor'),
    'fury-visual.bin': ('fury', 'appendFury'),
    'decor-fx-visual.bin': ('decor', 'appendDecor'),
    'events-anim.bin': ('events', 'appendConditional'),
}
CONFIGS = {
    'events-text.bin': 'appendText',
    'lore-tablets.bin': 'appendText',
    'nail.bin': 'appendSlash', 'crawler.bin': 'appendCrawlers',
    'shade.bin': 'appendShade', 'geo.bin': 'appendGeo',
    'geo-rocks.bin': 'appendGeo', 'health-cocoon.bin': 'appendGeo',
    'great-door.bin': 'appendDoor', 'fury-chest.bin': 'appendFury',
    'nail-interactions.bin': 'appendFury', 'decor.bin': 'appendDecor',
    'decor-effects.bin': 'appendDecor', 'contact-effects.bin': 'appendDecor',
    'hero-effects.bin': 'appendHeroEffects', 'focus-effects.bin': 'appendFocus',
    'initial-fall-fx.bin': 'appendDecor', 'owner-effects.bin': 'appendDecor',
    'stalactite-fx.bin': 'appendDecor', 'stalactite-visuals.bin': 'appendDecor',
    'ambient-particles.bin': 'appendRoom',
}
EXCLUDED = {
    'ability-art.bin': 'No spell/dash ability in the fresh Tutorial_01 session; other-room pickup art.',
    'cornifer-anim.bin': 'Cartographer is absent in Tutorial_01.',
    'collectibles.bin': 'Persistent collectible rules, no texels; Fury artwork submitted separately.',
    'tutorial.bin': 'Standalone fixture; full Tutorial_01 uses tutorial-room.bin.',
    'door.bin': 'Standalone fixture; full Tutorial_01 uses decor.bin.',
    'room-rules.bin': 'Room simulation metadata, no actor artwork.',
    'room-entities.bin': 'Spawn/simulation metadata, actor banks covered separately.',
    'tutorial-room.bin': 'Terrain simulation metadata, no actor artwork.',
    'static-room-visual.bin': 'Static room, plants and breakable bodies use existing appendRoom.',
    'static-plants.bin': 'Existing static room transform/visibility path.',
    'static-room-dynamics.bin': 'Existing static room transform/visibility path.',
    'static-room-ambient.bin': 'Existing static room transform path.',
    'static-grass.bin': 'Existing static room transform path; cut effects in decor atlas.',
    'plants.bin': 'Bind-time metadata; static-plants.bin selected for rendering.',
    'room-dynamics.bin': 'Nonstatic alternative; static package selected.',
    'room-ambient.bin': 'Nonstatic alternative; static package selected.',
    'grass.bin': 'Nonstatic alternative; static package selected.',
}


def members(root=ROOT):
    source = (root / 'src/main.cpp').read_text()
    start = source.index('    hk3ds::AnimationBank bank;')
    end = source.index('const bool lightingReady=', start)
    found = {}
    for match in re.finditer(r'romfs:/([a-z0-9-]+\.bin)', source[start:end]):
        name = match[1]
        found.setdefault(name, source.count('\n', 0, start+match.start())+1)
    return sorted(found.items())


def coverage_signature(root=ROOT):
    files = ['psp/src/actors.cpp', 'psp/src/draw_list.cpp', 'psp/src/game_loop.cpp',
             'psp/include/actors.hpp', 'psp/include/draw_list.hpp', 'tests/psp/test_actors.py']
    return hashlib.sha256(b''.join((root/p).read_bytes() for p in files)).hexdigest()


def report(root=ROOT, evidence=None):
    runtime = '\n'.join(p.read_text() for p in (root / 'psp/src').glob('*.cpp'))
    coverage = root/'build/psp/actors/coverage.json'
    quad_tested = coverage.exists() and json.loads(coverage.read_text())['signature'] == coverage_signature(root)
    trace, parity = set(), False
    if not evidence and (root/'build/psp/backend/last-run.txt').exists():
        evidence = root/'build/psp/backend'
    if evidence:
        trace = {line.split('\t')[0] for line in (evidence / 'oracle/assets.tsv').read_text().splitlines()}
        with (evidence / 'oracle/metrics.csv').open() as file:
            host = list(csv.DictReader(file))
        run = Path((evidence / 'last-run.txt').read_text().strip())
        with (run / 'metrics.csv').open() as file:
            psp = list(csv.DictReader(file))
        with (evidence/'frame-mae.csv').open() as file:
            comparisons = list(csv.DictReader(file))
        parity = (len(host) == len(psp) == 300 and len(comparisons) == 11 and
                  all(float(r['mae']) <= 8 for r in comparisons) and
                  all(a['quads'] == b['quads'] and abs(float(a['hero_x'])-float(b['hero_x'])) <= .01 and
                      abs(float(a['hero_y'])-float(b['hero_y'])) <= .01 for a, b in zip(host, psp)))
    rows = []
    for name, line in members(root):
        row = dict(table=name, source=f'src/main.cpp:{line}')
        if name in EXCLUDED:
            row.update(status='excluded', reason=EXCLUDED[name])
        elif name in ATLASES or name in CONFIGS:
            atlas, draw = ATLASES.get(name, (None, CONFIGS.get(name)))
            folder = root / '_private/converted-psp' / (atlas or '')
            converted = not atlas or (folder / ('hero.psp.bin' if atlas == 'hero' else name.replace('.bin', '.psp.bin'))).exists()
            drawn = draw+'(' in runtime and '"'+name+'"' in runtime
            tested = quad_tested and parity and name in trace
            row.update(transcoded=converted, drawn=drawn, parity_tested=tested,
                       status='green' if converted and drawn and tested else 'pending', draw=draw)
        else:
            row.update(status='unknown', reason='New resident table requires a rendering decision.')
        rows.append(row)
    return rows


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence', type=Path)
    parser.add_argument('--out', type=Path, default=ROOT / 'build/psp/actors-members.json')
    args = parser.parse_args()
    rows = report(evidence=args.evidence)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(rows, indent=2)+'\n')
    for row in rows:
        print(f"{row['status']:8} {row['table']:28} {row.get('draw', row.get('reason'))}")
    print(f"{sum(r['status'] in ('green', 'excluded') for r in rows)}/{len(rows)} accounted; {args.out}")
    raise SystemExit(any(r['status'] not in ('green', 'excluded') for r in rows))
