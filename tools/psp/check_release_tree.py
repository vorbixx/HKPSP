"""Release gate quick check: every script the player build runs must find its files in this tree.

Run with the builder bundle's Python from the tree that will be packed:
  runtime/python/python.exe -I tools/psp/check_release_tree.py [--root <tree>]

Checks, without any HK3DS data:
- PowerShell/batch/shell scripts: dot-sourced or Join-Path $PSScriptRoot '<x>.ps1|.py' files and
  quoted 'tools/...' script paths exist (2026-11-27: tools/error-mode.ps1 was never published).
- Python under tools/: every imported top-level module is the standard library, an installed
  package, or a local module next to the script, in tools/ or tools/psp/.
Exit code 1 lists every missing reference.
"""
import argparse
import ast
import importlib.util
import re
import sys
from pathlib import Path

SCRIPT_REF = re.compile(r"""Join-Path\s+\$PSScriptRoot\s+['"]([^'"]+\.(?:ps1|py))['"]""")
TOOL_PATH = re.compile(r"""['"](tools/[A-Za-z0-9_./-]+\.(?:ps1|py|sh|bat))['"]""")


def script_refs(root):
    missing = []
    for path in sorted(p for ext in ('*.ps1', '*.bat', '*.sh') for p in root.glob('tools/**/' + ext)):
        text = path.read_text(encoding='utf-8-sig', errors='replace')
        for name in SCRIPT_REF.findall(text):
            if not (path.parent / name).is_file():
                missing.append(f'{path.relative_to(root).as_posix()}: {name}')
        for name in TOOL_PATH.findall(text):
            if '*' not in name and not (root / name).is_file():
                missing.append(f'{path.relative_to(root).as_posix()}: {name}')
    return missing


def imported(tree):
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for alias in node.names:
                yield alias.name.split('.')[0], node.lineno
        elif isinstance(node, ast.ImportFrom) and node.module and not node.level:
            yield node.module.split('.')[0], node.lineno


def python_refs(root):
    missing, known = [], set(sys.stdlib_module_names) | set(sys.builtin_module_names)
    local_dirs = (root / 'tools', root / 'tools/psp')
    for path in sorted(root.glob('tools/**/*.py')):
        try:
            tree = ast.parse(path.read_text(encoding='utf-8-sig', errors='replace'))
        except SyntaxError as error:
            missing.append(f'{path.relative_to(root).as_posix()}: syntax error {error}')
            continue
        for name, line in sorted(set(imported(tree))):
            if name in known or name == '__future__':
                continue
            if any((d / (name + '.py')).is_file() or (d / name / '__init__.py').is_file()
                   for d in (path.parent, *local_dirs)):
                continue
            if importlib.util.find_spec(name) is not None:
                continue
            missing.append(f'{path.relative_to(root).as_posix()}:{line}: import {name}')
    return missing


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args(argv)
    root = args.root.resolve()
    missing = script_refs(root) + python_refs(root)
    for row in missing:
        print('MISSING', row)
    print(f'release tree check: {len(missing)} missing reference(s) in {root}')
    return int(bool(missing))


if __name__ == '__main__':
    raise SystemExit(main())
