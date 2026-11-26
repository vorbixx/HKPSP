"""Which Python runs the HK3DS tools: one answer, chosen automatically.

2026-11-09..12: the PATH Python (3.13) has no numpy; scene_lighting.py and 24 other converters
need it, and scene-* regen rules failed whenever regen.py was started without regen.ps1. The
interpreter is also part of every regen stamp's command signature, so a second interpreter marks
all rules as changed. Entry points therefore call `use_tool_python()`: it continues in the
canonical interpreter, or stops with what is missing.

Order: HK3DS_PYTHON, the bundled Codex runtime, then the running interpreter.
"""
from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys

REQUIRED = ('numpy', 'PIL')
REEXEC_MARK = 'HK3DS_TOOL_PYTHON_REEXEC'


class ToolchainError(RuntimeError):
    pass


def bundled_python() -> Path:
    return Path(os.environ.get('USERPROFILE') or Path.home()) / (
        '.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe')


def candidates() -> list[Path]:
    configured = os.environ.get('HK3DS_PYTHON')
    listed = ([Path(configured)] if configured else []) + [bundled_python(), Path(sys.executable)]
    return [path for index, path in enumerate(listed) if path.is_file() and path not in listed[:index]]


def has_modules(python: Path, modules=REQUIRED) -> bool:
    try:
        return subprocess.run([str(python), '-E', '-c', 'import ' + ', '.join(modules)],
                              capture_output=True, timeout=60).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def tool_python(modules=REQUIRED) -> Path:
    """The first candidate that imports every required module."""
    tried = candidates()
    for python in tried:
        if has_modules(python, modules):
            return python
    raise ToolchainError(
        f'No Python with {", ".join(modules)}: tried ' + ', '.join(str(p) for p in tried)
        + '. Set HK3DS_PYTHON to an interpreter that has them (the bundled Codex runtime does), or install them.')


def _same(left: Path, right: Path) -> bool:
    try:
        return os.path.samefile(left, right)
    except OSError:
        return False


def use_tool_python(modules=REQUIRED, argv=None) -> None:
    """Continue in the canonical interpreter: return if already there, otherwise run there and exit."""
    python = tool_python(modules)
    if _same(python, Path(sys.executable)):
        return
    if os.environ.get(REEXEC_MARK):
        raise ToolchainError(f'{sys.executable} was started as the tool Python but {python} is preferred; '
                             'check HK3DS_PYTHON')
    flags = ['-E'] if sys.flags.ignore_environment else []
    code = subprocess.run([str(python), *flags, *(sys.argv if argv is None else argv)],
                          env={**os.environ, REEXEC_MARK: '1'}).returncode
    raise SystemExit(code)


if __name__ == '__main__':
    try:
        print(tool_python())
    except ToolchainError as error:
        print(f'toolchain: {error}', file=sys.stderr)
        raise SystemExit(3)
