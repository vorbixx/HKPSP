"""Restore missing sidecar exports from hash-verified saved Tutorial_01 records.

Run through validation-lock.ps1 -Exclusive: research is shared across worktrees.
This never synthesizes original asset data or replaces a different existing file.
"""
import argparse
import hashlib
import json
from pathlib import Path

from extract_crawler import write_asset

ROOT = Path(__file__).resolve().parents[1]
EXPORTS = ('fury-mask', 'great-door', 'room-entities')


def restore(root, name, output=None):
    root = Path(root)
    research = root / '_private/research'
    tutorial = research / 'tutorial'
    inventory = json.loads((tutorial / 'inventory.json').read_text())
    hashes = inventory['source_sha256']
    manifest = None
    if name == 'fury-mask':
        visual = json.loads((research / 'room-visuals/room-visuals-map.json').read_text())
        if visual['build'] != 22529139 or hashes['level6-4092.json'] != visual['source_sha256']['level6-4092.json']:
            raise ValueError('Tutorial and Fury mask source builds differ')
        names = ['level6-4092.json', 'level6-12037.json', 'level6-8117.json']
        manifest = ('manifest.json', dict(build=visual['build'], source_sha256={n: hashes[n] for n in names}))
    elif name == 'great-door':
        visual = json.loads((research / 'room-visuals/room-visuals-map.json').read_text())
        if visual['build'] != 22529139 or hashes['level6-1806.json'] != visual['source_sha256']['level6-1806.json']:
            raise ValueError('Tutorial and Great Door source builds differ')
        rows = [r for r in inventory['rows'] if r['go'] == 'level6-1806.json' and r['type'] == 'BoxCollider2D']
        if len(rows) != 1:
            raise ValueError('Expected one saved Great Door collider')
        parts = rows[0]['parts']
        names = sorted({n for values in parts.values() for n in values})
        manifest = ('sources.json', dict(build_id=visual['build'], parts=parts, sha256={n: hashes[n] for n in names}))
    elif name == 'room-entities':
        names = ['sharedassets6.assets-1150.json']
        previous = json.loads((root / '_private/converted/tutorial/room-entities-manifest.json').read_text())
        if previous['build_id'] != 22529139:
            raise ValueError('Room entity source build differs')
        key = '_private/research/room-entities/' + names[0]
        hashes = {Path(k.replace('\\', '/')).name: v for k, v in previous['source_sha256'].items()
                  if k.replace('\\', '/') == key}
    else:
        raise ValueError('Unknown saved export: ' + name)
    files = {}
    for filename in names:
        raw = (tutorial / filename).read_bytes()
        if hashlib.sha256(raw).hexdigest() != hashes[filename]:
            raise ValueError('Saved original source changed: ' + filename)
        files[filename] = raw
    if manifest:
        files[manifest[0]] = (json.dumps(manifest[1], indent=2) + '\n').encode()
    output = research / name if output is None else output
    # Validate every source and destination before modifying shared research.
    for filename, raw in files.items():
        target = output / filename
        if target.exists() and target.read_bytes() != raw:
            raise ValueError('Refusing to replace different existing export: ' + str(target))
    for filename, raw in files.items():
        if not (output / filename).exists():
            write_asset(output / filename, raw)
    print(f'{name}: restored {len(names)} original records to {output}; SHA-256 verified')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--only', required=True, choices=EXPORTS)
    restore(ROOT, parser.parse_args().only)
