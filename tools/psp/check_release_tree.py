"""Release gate quick check: every script the player build runs must find its files in this tree.

Run with the builder bundle's Python from the tree that will be packed:
  runtime/python/python.exe -I tools/psp/check_release_tree.py [--root <tree>]

Checks, without any HK3DS data:
- PowerShell/batch/shell scripts: dot-sourced or Join-Path $PSScriptRoot '<x>.ps1|.py' files and
  quoted 'tools/...' script paths exist (2026-11-27: tools/error-mode.ps1 was never published).
- Python under tools/: every imported top-level module is the standard library, an installed
  package, or a local module next to the script, in tools/ or tools/psp/.
- Windows binaries under runtime/: every imported DLL ships in the tree or comes with a clean
  Windows (no Visual C++ Redistributable).
Exit code 1 lists every missing reference.
"""
import argparse
import ast
import importlib.util
import os
import re
import struct
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


# System32 has these only after the Visual C++ Redistributable is installed; a clean Windows lacks them.
VC_REDIST = {'msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll', 'msvcp140_atomic_wait.dll',
             'msvcp140_codecvt_ids.dll', 'vcruntime140.dll', 'vcruntime140_1.dll', 'vcomp140.dll',
             'concrt140.dll', 'vccorlib140.dll', 'msvcr120.dll', 'msvcp120.dll', 'msvcr100.dll', 'msvcp100.dll'}


def pe_imports(path):
    """DLL names in a PE file's import table."""
    data = path.read_bytes()
    if data[:2] != b'MZ':
        return []
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    sections, optional_size = struct.unpack_from('<H', data, pe + 6)[0], struct.unpack_from('<H', data, pe + 20)[0]
    optional = pe + 24
    directory = optional + (112 if struct.unpack_from('<H', data, optional)[0] == 0x20b else 96)
    table = struct.unpack_from('<I', data, directory + 8)[0]
    spans = []
    for index in range(sections):
        size, address, raw_size, raw = struct.unpack_from('<IIII', data, optional + optional_size + index * 40 + 8)
        spans.append((address, max(size, raw_size), raw))

    def offset(rva):
        return next((rva - a + r for a, s, r in spans if a <= rva < a + s), None)
    names, entry = [], offset(table) if table else None
    while entry is not None and entry + 20 <= len(data):
        name = struct.unpack_from('<I', data, entry + 12)[0]
        if not name:
            break
        start = offset(name)
        names.append(data[start:data.index(b'\0', start)].decode('ascii', 'replace'))
        entry += 20
    return names


def dll_refs(root):
    """Windows binaries under runtime/: every imported DLL is next to it, in the Python folder (for
    Python extensions, with the wheels' .libs folders) or part of a clean Windows (2026-11-27:
    astc_encoder needed msvcp140.dll, which only the Visual C++ Redistributable installs)."""
    if os.name != 'nt' or not (root / 'runtime').is_dir():
        return []
    system = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32'
    python = root / 'runtime/python'
    python_dirs = [python, *(python / 'Lib/site-packages').glob('*.libs')]
    missing = []
    for path in sorted(p for ext in ('*.dll', '*.pyd', '*.exe') for p in (root / 'runtime').rglob(ext)):
        try:
            names = pe_imports(path)
        except (struct.error, ValueError, OSError):
            continue
        dirs = [path.parent] + (python_dirs if python in path.parents else [])
        for name in names:
            low = name.lower()
            if low.startswith(('api-ms-', 'ext-ms-')) or any((d / name).is_file() for d in dirs):
                continue
            if low not in VC_REDIST and (system / name).is_file():
                continue
            missing.append(f'{path.relative_to(root).as_posix()}: DLL {name}')
    return missing


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args(argv)
    root = args.root.resolve()
    missing = script_refs(root) + python_refs(root) + dll_refs(root)
    for row in missing:
        print('MISSING', row)
    print(f'release tree check: {len(missing)} missing reference(s) in {root}')
    return int(bool(missing))


if __name__ == '__main__':
    raise SystemExit(main())
