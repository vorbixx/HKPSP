"""Byte-preserving cache for the converters' single-PNG tex3ds invocations."""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys

from converted_files import internal_access, unlink_converted
from regen_cache import OutputCache, graph_lock

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = 'texture.t3x'
# Player builds (HKPSP_TRIM_3DS_TEXTURES=1, set by tools/psp/hkpsp_build.py) keep only this
# prefix of each .t3x: the PSP never reads 3DS textures, and every tool that does read one
# (check_memory_budget, static_decor_bake, convert_scenes) reads only the 4-byte header or
# checks that the file is not empty. Saves about 17 GB of the full build.
TRIM_BYTES = 64


def trim_output(command, cwd):
    """Cut a finished .t3x to its header in player builds."""
    if os.environ.get('HKPSP_TRIM_3DS_TEXTURES') != '1' or not isinstance(command, (list, tuple)):
        return
    args = [os.fspath(arg) for arg in command]
    if '-o' not in args[:-1]:
        return
    destination = Path(cwd) / args[args.index('-o') + 1]
    if destination.suffix.lower() == '.t3x' and destination.is_file() and destination.stat().st_size > TRIM_BYTES:
        header = destination.read_bytes()[:TRIM_BYTES]
        destination.unlink()  # never write through a hardlink shared with another tree
        destination.write_bytes(header)


# tex3ds texture formats (GPU_TEXCOLOR); a player build only reads byte 3 of each .t3x.
FORMAT_CODES = {'rgba8': 0, 'rgb8': 1, 'rgba5551': 2, 'rgb565': 3, 'rgba4': 4, 'la8': 5, 'hilo8': 6,
                'l8': 7, 'a8': 8, 'la4': 9, 'l4': 10, 'a4': 11, 'etc1': 12, 'etc1a4': 13}


def stub_output(command, cwd):
    """Player builds: write the trimmed .t3x header without running tex3ds.

    The PSP never reads 3DS textures; tools/static_decor_bake.py reads only the format byte and
    the rest only checks the file exists. tex3ds was 20 of 45 s of a scene conversion
    (Abyss_19, cache off, 2026-11-27)."""
    if os.environ.get('HKPSP_STUB_3DS_TEXTURES') != '1' or not isinstance(command, (list, tuple)):
        return None
    args = [os.fspath(arg) for arg in command]
    if '-o' not in args[:-1] or '-f' not in args[:-1]:
        return None
    code = FORMAT_CODES.get(args[args.index('-f') + 1].lower())
    destination = Path(cwd) / args[args.index('-o') + 1]
    source = Path(cwd) / args[-1]
    if code is None or destination.suffix.lower() != '.t3x' or not source.is_file():
        return None
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        destination.unlink()  # never write through a hardlink shared with another tree
    # Byte 2 holds log2(size)-3 of the power-of-two texture (width bits 0-2, height bits 3-5),
    # read by tools/check_memory_budget.py; take it from the PNG header as tex3ds does.
    with source.open('rb') as stream:
        png = stream.read(24)
    if png[:8] != bytes.fromhex('89504e470d0a1a0a'):
        return None
    width, height = int.from_bytes(png[16:20], 'big'), int.from_bytes(png[20:24], 'big')
    def bits(size):
        return max(0, min(7, (max(size, 8) - 1).bit_length() - 3))
    destination.write_bytes(bytes((1, 0, bits(width) | bits(height) << 3, code)) + bytes(TRIM_BYTES - 4))
    return subprocess.CompletedProcess(command, 0, b'', b'')


def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def invocation(command, cwd):
    """Only cache the supported shape; future tex3ds modes run normally."""
    args = [os.fspath(arg) for arg in command]
    if (len(args) != 8 or args[1] != '-f' or args[3] != '-z'
            or args[5] != '-o' or Path(args[7]).suffix.lower() != '.png'):
        return None
    binary = shutil.which(args[0])
    if binary is None:
        binary = cwd / args[0]
    source, destination = cwd / args[7], cwd / args[6]
    # Operand paths do not affect .t3x bytes. Normalize only those two operands
    # so identical PNGs can share entries across directories and worktrees.
    identity = dict(kind='tex3ds', schema=1, png=sha256(source),
                    arguments=[*args[1:6], '<output>', '<input>'],
                    binary=sha256(Path(binary)))
    return identity, source, destination, Path(binary)


def run(command, *, root=ROOT, directory=None, **kwargs):
    """subprocess.run subset used by converters; cache failures are harmless."""
    cache = info = None
    allowed = {'check', 'capture_output', 'stdout', 'stderr', 'text', 'encoding',
               'errors', 'universal_newlines', 'timeout', 'cwd'}
    enabled = (os.environ.get('HK3DS_REGEN_CACHE') != '0'
               and os.environ.get('HK3DS_TEX3DS_CACHE') != '0')
    if enabled and isinstance(command, (list, tuple)) and set(kwargs) <= allowed:
        try:
            info = invocation(command, Path(kwargs.get('cwd') or Path.cwd()))
            if info is not None:
                identity, source, destination, binary = info
                protected = Path(os.path.abspath(destination)).is_relative_to(
                    Path(os.path.abspath(root)) / '_private/converted')
                # The logical cache name has no converted prefix. Preserve sharing
                # for protected files; arbitrary caller destinations remain copies.
                if protected:
                    unlink_converted(destination)
                sys.audit('open', os.fspath(destination), 'wb', os.O_WRONLY | os.O_CREAT | os.O_TRUNC)
                def validate(record):
                    return (isinstance(record, dict) and isinstance(record.get('outputs'), dict)
                            and set(record['outputs']) == {OUTPUT})
                with internal_access():
                    cache = OutputCache(root, graph_lock, sha256, directory=directory,
                                        files={OUTPUT: destination},
                                        shared_files=(OUTPUT,) if protected else ())
                    restored = cache.restore(identity, validate)
                if restored is not None:
                    empty = '' if (kwargs.get('text') or kwargs.get('encoding')
                                   or kwargs.get('universal_newlines')) else b''
                    stdout = empty if (kwargs.get('capture_output') or
                                       kwargs.get('stdout') == subprocess.PIPE) else None
                    stderr = empty if (kwargs.get('capture_output') or
                                       kwargs.get('stderr') == subprocess.PIPE) else None
                    return subprocess.CompletedProcess(command, 0, stdout, stderr)
        except (OSError, ValueError, TypeError):
            cache = None
    stubbed = stub_output(command, kwargs.get('cwd') or Path.cwd())
    if stubbed is not None:
        return stubbed
    result = subprocess.run(command, **kwargs)
    if result.returncode == 0 and os.environ.get('HKPSP_TRIM_3DS_TEXTURES') == '1':
        trim_output(command, kwargs.get('cwd') or Path.cwd())
        cache = None  # a trimmed texture must never reach the shared cache
    if cache is not None and result.returncode == 0:
        try:
            identity, source, destination, binary = info
            if sha256(source) == identity['png'] and sha256(binary) == identity['binary']:
                record = {'outputs': {OUTPUT: sha256(destination)}}
                with internal_access():
                    cache.publish(identity, record)
        except (OSError, ValueError, TypeError):
            pass  # A full/unavailable cache must not break successful conversion.
    return result
