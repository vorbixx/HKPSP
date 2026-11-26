"""Read-through packs for saved scene exports (offline converters only).

2026-11-01 measurement: 99% of a scene rule's reads are its ~12.5k small export files under
_private/research/crossroads-scenes/<Scene>/ (scene-Fungus3_archive_02: 12,525 of 12,682 reads).
On PC2's SATA SSD a cold open costs ~3.5 ms (Ruins2_06: 5,733 files in 19.9 s cold, 1.5 s warm),
so a 773-rule regen spent most of its 10.7 h of conversion opening files. A pack holds one export
directory in one file (zlib per entry) and serves reads from memory.

Safety: a pack is only a cache. It is keyed by the directory's listing signature (relative names,
sizes, mtimes, as tools/regen.py walk_directory), checked once per process before any read is
served; a stale or unreadable pack is rebuilt or bypassed. Served reads raise the same 'open'
audit event, so regen_worker records identical accesses. Writes and non-read modes never use packs.
Enabled only when HK3DS_EXPORT_PACKS=1 (tools/regen.py sets it for converters)."""
from __future__ import annotations

import io
import json
import os
from pathlib import Path
import struct
import sys
import threading
import zlib

ROOT = Path(__file__).resolve().parents[1]
EXPORT_ROOT = '_private/research/crossroads-scenes'
PACK_DIRECTORY = '_private/regen-cache/export-packs'
MAGIC = b'HKXP1\0\0\0'
READ_MODES = ('r', 'rt', 'tr', 'rb', 'br')
MIN_FREE_BYTES = 15 * 1024 ** 3
_lock = threading.RLock()
_packs: dict[str, 'Pack | None'] = {}


def listing(directory: Path):
    """Sorted (relative posix name, size, mtime_ns) for every file below directory."""
    found, pending = [], [str(directory)]
    base = len(str(directory)) + 1
    while pending:
        with os.scandir(pending.pop()) as entries:
            for entry in entries:
                if entry.name == '__pycache__':
                    continue
                if entry.is_dir(follow_symlinks=False):
                    pending.append(entry.path)
                elif entry.is_file():
                    stat = entry.stat()
                    found.append((entry.path[base:].replace('\\', '/'), stat.st_size, stat.st_mtime_ns))
    found.sort()
    return found


def signature(entries) -> str:
    import hashlib
    return hashlib.sha256('\n'.join(f'{n}\t{s}\t{m}' for n, s, m in entries).encode()).hexdigest()


class Pack:
    def __init__(self, path: Path, index: dict, data_start: int):
        self.path, self.index, self.data_start = path, index, data_start
        self.handle, self.lock, self.owner = None, threading.Lock(), None

    def read(self, name: str) -> bytes | None:
        entry = self.index.get(name)
        if entry is None:
            return None
        offset, stored, size = entry
        with self.lock:
            # One handle per pack for the process: reopening it per entry would bring back the
            # per-open cost the pack exists to remove. A forked worker inherits the parent's
            # descriptor and its shared file offset, so it opens its own, and reads are
            # positional where the platform has pread.
            if self.handle is None or self.owner != os.getpid():
                self.handle, self.owner = open_raw(self.path), os.getpid()
            start = self.data_start + offset
            if hasattr(os, 'pread'):
                raw = os.pread(self.handle.fileno(), stored, start)
            else:
                self.handle.seek(start)
                raw = self.handle.read(stored)
            if len(raw) != stored:
                raise OSError(f'export pack {self.path}: {name} is truncated')
            data = zlib.decompress(raw)
        if len(data) != size:
            raise OSError(f'export pack {self.path}: {name} has {len(data)} bytes, expected {size}')
        return data


def open_raw(path):
    from converted_files import internal_access
    with internal_access():
        return _original_open(path, 'rb')


def load(path: Path, expected: str):
    try:
        with open_raw(path) as handle:
            if handle.read(8) != MAGIC:
                return None
            length = struct.unpack('<Q', handle.read(8))[0]
            header = json.loads(handle.read(length).decode('utf-8'))
    except (OSError, ValueError):
        return None
    if header.get('signature') != expected or not isinstance(header.get('files'), dict):
        return None
    return Pack(path, header['files'], 16 + length)


def build(directory: Path, path: Path, entries, expected: str):
    """Write a pack of directory's current files; None if the listing changed while reading."""
    from converted_files import internal_access
    import shutil
    index, chunks, offset = {}, [], 0
    # All packs together take several GB (about 20 MB per scene); never let them fill a drive.
    probe = next(p for p in path.parents if p.is_dir())
    if shutil.disk_usage(probe).free < MIN_FREE_BYTES:
        return None
    with internal_access():
        for name, size, _ in entries:
            with _original_open(directory / name, 'rb') as handle:
                data = handle.read()
            if len(data) != size:
                return None
            stored = zlib.compress(data, 1)
            index[name] = (offset, len(stored), size)
            chunks.append(stored)
            offset += len(stored)
        if signature(listing(directory)) != expected:
            return None
        header = json.dumps(dict(signature=expected, files=index), separators=(',', ':')).encode('utf-8')
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_name(f'{path.name}.{os.getpid()}.{threading.get_ident()}.tmp')
        with _original_open(temporary, 'wb') as handle:
            handle.write(MAGIC + struct.pack('<Q', len(header)) + header)
            for chunk in chunks:
                handle.write(chunk)
        os.replace(temporary, path)
    return Pack(path, index, 16 + len(header))


def pack_root(scene_directory: Path) -> Path:
    """The checkout whose packs serve this export directory: the one that physically holds it.
    Worktree exports are junctions into main's (2026-11-01), so worktrees share main's ~6 GB of packs
    instead of building their own; the listing signature still decides whether a pack is valid."""
    real = Path(os.path.realpath(scene_directory))
    if real.parent.as_posix().lower().endswith(EXPORT_ROOT.lower()):
        return real.parents[3]
    return ROOT


def pack_for(scene_directory: Path):
    key = os.path.normcase(str(scene_directory))
    with _lock:
        if key in _packs:
            return _packs[key]
        _packs[key] = None  # Recursion guard and the result if anything below fails.
        try:
            entries = listing(scene_directory)
            expected = signature(entries)
            path = pack_root(scene_directory) / PACK_DIRECTORY / f'{scene_directory.name}.pack'
            pack = load(path, expected) or build(scene_directory, path, entries, expected)
        except OSError:
            pack = None
        _packs[key] = pack
        return pack


def locate(file):
    """(scene export directory, relative name) for a file inside a packed export, else None."""
    if not isinstance(file, (str, bytes, os.PathLike)):
        return None
    try:
        path = Path(os.path.abspath(os.fsdecode(file)))
        relative = path.relative_to(ROOT / EXPORT_ROOT)
    except (ValueError, TypeError):
        return None
    parts = relative.parts
    if len(parts) < 2:
        return None
    return ROOT / EXPORT_ROOT / parts[0], '/'.join(parts[1:]), path


def _wrap(original):
    def open_packed(file, mode='r', buffering=-1, encoding=None, errors=None,
                    newline=None, closefd=True, opener=None):
        if opener is None and isinstance(mode, str) and mode in READ_MODES:
            found = locate(file)
            if found is not None:
                directory, name, path = found
                pack = pack_for(directory)
                data = pack.read(name) if pack is not None else None
                if data is not None:
                    sys.audit('open', str(path), mode, os.O_RDONLY)
                    raw = io.BytesIO(data)
                    if 'b' in mode:
                        return raw
                    return io.TextIOWrapper(raw, encoding=io.text_encoding(encoding), errors=errors,
                                            newline=newline)
        return original(file, mode, buffering, encoding, errors, newline, closefd, opener)
    return open_packed


_original_open = None


def install():
    """Serve export reads from packs in this process (idempotent)."""
    global _original_open
    import builtins
    import converted_files  # noqa: F401  Its open wrappers must sit below this one.
    if getattr(sys, '_hk3ds_export_packs_installed', False):
        return
    _original_open = io.open
    builtins.open = _wrap(builtins.open)
    io.open = _wrap(io.open)
    sys._hk3ds_export_packs_installed = True


if os.environ.get('HK3DS_EXPORT_PACKS') == '1':
    install()
