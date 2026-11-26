"""Records which converter code produced each regen-assets group, and lists the stale groups.

After a merge that changed a converter, its output in _private/converted-psp stayed old until someone
remembered to rerun that group (map, slyshop and audio on 2026-11-19 alone). regen-assets.ps1 now
stamps every group it runs with a hash of the converter script and the local modules it imports; a
group whose code hash differs from its stamp, or that has no stamp, is stale.

  python -I tools/psp/regen_stamp.py stale  NAME=SCRIPT ...   prints stale group names, one per line
  python -I tools/psp/regen_stamp.py record NAME=SCRIPT       stamps one group after a successful run

The stamps live next to the data (_private/converted-psp/.regen-stamps.json), so a worktree that
copies the converted tree inherits the stamps that match its copy.
"""
from __future__ import annotations

import hashlib
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
STAMPS = ROOT / '_private' / 'converted-psp' / '.regen-stamps.json'
SEARCH = (ROOT / 'tools' / 'psp', ROOT / 'tools')
IMPORT = re.compile(r'^\s*(?:from\s+([\w.]+)\s+import|import\s+([\w.]+(?:\s*,\s*[\w.]+)*))', re.M)


def local_modules(script: Path) -> list[Path]:
    """The script and every tools/ or tools/psp/ module it imports, transitively."""
    seen: dict[Path, None] = {}
    todo = [script.resolve()]
    while todo:
        path = todo.pop()
        if path in seen or not path.is_file():
            continue
        seen[path] = None
        for frm, imp in IMPORT.findall(path.read_text(encoding='utf-8', errors='replace')):
            for name in ([frm] if frm else [n.strip() for n in imp.split(',')]):
                head = name.split('.')[0]
                for folder in SEARCH:
                    candidate = folder / f'{head}.py'
                    if candidate.is_file():
                        todo.append(candidate.resolve())
                        break
    return sorted(seen)


def code_hash(script: Path) -> str:
    digest = hashlib.sha256()
    for path in local_modules(script):
        digest.update(path.relative_to(ROOT).as_posix().encode())
        digest.update(path.read_bytes().replace(b'\r\n', b'\n'))
    return digest.hexdigest()[:16]


def load() -> dict:
    try:
        return json.loads(STAMPS.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return {}


def parse(pairs: list[str]) -> list[tuple[str, Path]]:
    out = []
    for pair in pairs:
        name, _, script = pair.partition('=')
        if not name or not script:
            raise SystemExit(f'expected NAME=SCRIPT, got {pair!r}')
        out.append((name, ROOT / script))
    return out


def main(argv: list[str]) -> int:
    if len(argv) < 2 or argv[0] not in ('stale', 'record'):
        print(__doc__)
        return 2
    stamps = load()
    groups = parse(argv[1:])
    if argv[0] == 'stale':
        for name, script in groups:
            if stamps.get(name, {}).get('code') != code_hash(script):
                print(name)
        return 0
    for name, script in groups:
        stamps[name] = {'code': code_hash(script), 'script': script.relative_to(ROOT).as_posix()}
    if not STAMPS.parent.is_dir():
        raise SystemExit(f'no converted tree at {STAMPS.parent}')
    STAMPS.write_text(json.dumps(stamps, indent=1, sort_keys=True) + '\n', encoding='utf-8')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
