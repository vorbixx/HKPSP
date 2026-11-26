"""Disposable verified generations, shared with copy-on-write converter outputs."""
from __future__ import annotations

from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import shutil
import time
import uuid

import converted_files  # Install copy-on-write before restoring shared outputs.
from converted_tree import install_file

SCHEMA = 1
GIB = 1024 ** 3
# Leave room for compiler objects/staging. Same-volume publication allocates only
# directory entries and metadata; reserving another full asset copy wasted 15 GiB.
MIN_FREE = 2 * GIB
# Eviction keeps this much free on the cache's drive: 2026-10-27 the 12 GiB cache grew to 11.8 GiB
# while 14 agents regenerated and C: fell to 1.5 GiB. A 12 GiB reserve then emptied the cache on every
# publish once C: stayed below 12 GiB (no hits, 30-60 min regens), so the reserve is 4 GiB.
RESERVE_FREE = int(float(os.environ.get('HK3DS_REGEN_CACHE_RESERVE_GB', '4')) * GIB)
_ENTRY_SIZES = {}  # entry name -> (measured monotonic time, bytes owned only by the cache, of which data)
OWNED_RECHECK_SECONDS = 300.0
EVICT_SCAN_SECONDS = 3600.0  # 2026-11-07: 600 s was shorter than one ~16 000-entry scan (see evict)


def _owned_bytes(name, path):
    """(bytes no other link holds, of which output data): what deleting the entry frees on disk.

    2026-10-31: entries are hardlinks to converted outputs; a full regen rewrites those outputs, and the
    cache kept the superseded versions while counting every entry at its logical size, so C: fell to
    2.4 GB under a 32 GB limit. Link counts change as outputs are rewritten: recheck every few minutes."""
    known = _ENTRY_SIZES.get(name)
    if known is not None and time.monotonic() - known[0] < OWNED_RECHECK_SECONDS:
        return known[1], known[2]
    owned = data = 0
    with os.scandir(path) as files:
        for file in files:
            if file.is_file():
                stat = os.stat(file.path)  # DirEntry.stat() has no link count on Windows
                if stat.st_nlink <= 1:
                    owned += stat.st_size
                    if file.name != 'entry.json':
                        data += stat.st_size
    _ENTRY_SIZES[name] = (time.monotonic(), owned, data)
    return owned, data


def cache_directory(root: Path) -> Path:
    """Follow the same main-checkout location as validation-lock.ps1."""
    main = root
    dotgit = root / '.git'
    if dotgit.is_file():
        gitdir = (root / dotgit.read_text(encoding='utf-8').strip().removeprefix('gitdir:').strip()).resolve()
        common = gitdir / 'commondir'
        main = ((gitdir / common.read_text(encoding='utf-8').strip()).resolve().parent
                if common.is_file() else gitdir.parents[2])
    # v2 (2026-10-27): entries written by sandboxed agents with the old owner-only staging stay
    # unreadable to everyone else and blocked their keys for good (12 restores, 56 failed publishes in
    # one Changed run); a fresh namespace leaves them behind.
    return main / '_private/regen-cache/v2'


def cache_key(identity):
    return hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()


def relative_file(name):
    path = Path(name)
    return (bool(name) and not path.anchor and '..' not in path.parts
            and path.as_posix() == name and not name.startswith('missing:'))


@contextmanager
def graph_lock(path: Path, timeout=3600):
    """OS releases the lock on a crash; concurrent checks cannot regenerate together."""
    path.parent.mkdir(parents=True, exist_ok=True)
    start = time.monotonic()
    while True:
        try:
            descriptor = os.open(path, os.O_CREAT | os.O_RDWR)
            try:
                if not os.fstat(descriptor).st_size:
                    os.lseek(descriptor, 0, os.SEEK_SET)
                    os.write(descriptor, b'0')
                initialized = os.fstat(descriptor).st_size > 0
            finally:
                os.close(descriptor)
        except PermissionError:
            initialized = False
        if initialized:
            break
        if time.monotonic() - start >= timeout:
            raise TimeoutError('Timed out initializing asset regeneration lock')
        time.sleep(0.01)
    with path.open('a+b') as lock:
        while True:
            try:
                lock.seek(0)
                if os.name == 'nt':
                    import msvcrt
                    msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
                else:
                    import fcntl
                    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except OSError:
                if time.monotonic() - start >= timeout:
                    raise TimeoutError('Timed out waiting for asset regeneration')
                time.sleep(0.25)
        try:
            yield
        finally:
            lock.seek(0)
            if os.name == 'nt':
                import msvcrt
                msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                import fcntl
                fcntl.flock(lock, fcntl.LOCK_UN)


STALE_STAGE_SECONDS = 3600


def _process_alive(pid):
    if pid == os.getpid():
        return True
    if os.name != 'nt':
        try:
            os.kill(pid, 0)
            return True
        except ProcessLookupError:
            return False
        except PermissionError:
            return True
    import ctypes
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    handle = kernel.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not handle:
        return ctypes.get_last_error() == 5  # Access denied: another user's live process.
    try:
        code = ctypes.c_ulong()
        return bool(kernel.GetExitCodeProcess(handle, ctypes.byref(code))) and code.value == 259  # STILL_ACTIVE
    finally:
        kernel.CloseHandle(handle)


def clear_stale_stages(directory, keep=None):
    """Remove staging directories of dead writers (pending-<pid>-<id>), or any untouched for an hour."""
    stale = time.time() - STALE_STAGE_SECONDS
    for path in directory.glob('pending-*'):
        try:
            if path == keep or not path.is_dir():
                continue
            owner = path.name.split('-')[1]
            if (not owner.isdigit() or not _process_alive(int(owner))
                    or path.stat().st_mtime < stale):
                shutil.rmtree(path, ignore_errors=True)
        except OSError:
            continue


class OutputCache:
    def __init__(self, root, lock, digest, *, directory=None, enabled=True, files=None, shared_files=()):
        self.root, self.lock, self.digest = root, lock, digest
        # Stable logical output names can map to different physical paths in
        # standalone converter invocations and other worktrees.
        self.files = files or {}
        # Explicit aliases are used only for outputs protected by converted_files.
        self.shared_files = frozenset(shared_files)
        self.directory = Path(directory) if directory is not None else cache_directory(root)
        self.enabled = enabled and os.environ.get('HK3DS_REGEN_CACHE') != '0'
        self.limit = int(float(os.environ.get('HK3DS_REGEN_CACHE_MAX_GB', '32')) * GIB)

    def shares(self, name):
        return name.startswith('_private/converted/') or name in self.shared_files

    def file(self, name):
        return self.files.get(name, self.root / name)

    def has_entry(self, identity):
        """Cheap check that an entry for `identity` exists; restore() still validates every byte."""
        if not self.enabled:
            return False
        try:
            metadata = json.loads((self.directory / cache_key(identity) / 'entry.json').read_text(encoding='utf-8'))
            return metadata.get('schema') == SCHEMA and metadata.get('identity') == identity
        except (OSError, ValueError):
            return False

    def restore(self, identity, validate):
        """Validate the complete entry before installing; caller verifies restored outputs."""
        if not self.enabled or not self.directory.is_dir():
            return None
        key = cache_key(identity)
        # 2026-11-01: restore and publish both held the machine-wide cache.lock while hashing and copying;
        # a profiled scene rule (Fungus1_19) slept 126 of its 158 s waiting for it behind other workers and
        # worktrees. Entries appear only by atomic rename and leave only by eviction, so a restore needs no
        # lock: an entry evicted mid-restore fails like a miss and the caller verifies restored outputs.
        entry = self.directory / key
        try:
            metadata = json.loads((entry / 'entry.json').read_text(encoding='utf-8'))
            record = metadata['record']
            if metadata['schema'] != SCHEMA or metadata['identity'] != identity or not validate(record):
                raise ValueError('invalid cache record')
            for index, (name, expected) in enumerate(record['outputs'].items()):
                source = entry / f'{index}.bin'
                if source.is_symlink() or self.digest(source) != expected:
                    raise ValueError('corrupt cache bytes')
            for index, name in enumerate(record['outputs']):
                destination = self.file(name)
                destination.parent.mkdir(parents=True, exist_ok=True)
                install_file(entry / f'{index}.bin', destination,
                             share=self.shares(name))
            os.utime(entry, None)
            return record
        except OSError:
            return None  # Missing, or evicted while restoring: a miss, never deleted here.
        except (ValueError, TypeError, KeyError, AttributeError):
            # Corrupt entries are misses and must be replaceable by a real run.
            with self.lock(self.directory / 'cache.lock'):
                if entry.is_dir():
                    shutil.rmtree(entry, ignore_errors=True)
            return None

    def publish(self, identity, record):
        if not self.enabled:
            return
        self.directory.mkdir(parents=True, exist_ok=True)
        # A killed writer can leave staging files. Clear these even under disk pressure, before the
        # space gate can skip this publication.
        clear_stale_stages(self.directory)
        entry = self.directory / cache_key(identity)
        if entry.exists():
            return  # Another checkout published this generation first.
        metadata = json.dumps(dict(schema=SCHEMA, identity=identity, record=record)).encode('utf-8')
        size = sum(self.file(name).stat().st_size for name in record['outputs']) + len(metadata)
        if size > self.limit:
            return
        allocation = len(metadata) + 4096 * len(record['outputs'])
        allocation += sum(self.file(name).stat().st_size for name in record['outputs']
                          if (not self.shares(name) or
                              self.file(name).stat().st_dev != self.directory.stat().st_dev))
        reserve = max(MIN_FREE, RESERVE_FREE)
        if shutil.disk_usage(self.directory).free - allocation < reserve:
            return
        # Staging (copy + hash) runs outside cache.lock; only eviction and the final rename take it
        # (see restore). Not tempfile: since Python 3.12.4 its 0o700 directories get an owner-only ACL on
        # Windows, so entries published by sandboxed Codex agents (another user SID) were unreadable here
        # and every later publish failed in evict (2026-10-27: 0 cache hits). A plain mkdir inherits the
        # cache directory's shared ACL.
        stage = self.directory / f'pending-{os.getpid()}-{uuid.uuid4().hex}'
        stage.mkdir()
        try:
            for index, (name, expected) in enumerate(record['outputs'].items()):
                target = stage / f'{index}.bin'
                install_file(self.file(name), target,
                             share=self.shares(name))
                if self.digest(target) != expected:
                    return  # Outputs changed after the generation was stamped.
                if shutil.disk_usage(self.directory).free < reserve:
                    return  # A filesystem without hardlinks may have copied the file.
            (stage / 'entry.json').write_bytes(metadata)
            with self.lock(self.directory / 'cache.lock'):
                if entry.exists():
                    return
                # The lock prevents two evictions and a publish from racing over the same space.
                self.evict(size)
                if shutil.disk_usage(self.directory).free < reserve:
                    return  # Other worktrees can consume disk while we copy.
                stage.rename(entry)
        finally:
            if stage.exists():
                shutil.rmtree(stage, ignore_errors=True)

    def evict(self, incoming):
        # 2026-11-04: the full regen stalled 75 min: one worker held cache.lock re-measuring all 12 550
        # entries (23 580 metadata ops per 15 s) while 7 others waited, and nothing was deleted. With the
        # default limit and ample free disk, a full scan runs at most once per EVICT_SCAN_SECONDS per
        # machine (stamp file in the cache directory); small test limits always scan.
        stamp = self.directory / 'evict-scan.stamp'
        if self.limit >= 16 * GIB:
            try:
                recent = time.time() - stamp.stat().st_mtime < EVICT_SCAN_SECONDS
            except OSError:
                recent = False
            if recent and shutil.disk_usage(self.directory).free - incoming > RESERVE_FREE + 8 * GIB:
                return
            try:
                stamp.touch()
            except OSError:
                pass
        # 2026-10-30 every publish statted each file of ~900 entries: 4-5 s per tex3ds page, 70 s of a
        # 264 s scene. Directory listings carry size and mtime on Windows, and an entry's size never
        # changes after its atomic rename, so each process measures an entry once.
        entries = []
        with os.scandir(self.directory) as listing:
            for item in listing:
                name = item.name
                if len(name) != 64 or not all(c in '0123456789abcdef' for c in name):
                    continue
                try:
                    if not item.is_dir():
                        continue
                    size, data = _owned_bytes(name, item.path)
                    entries.append((item.stat().st_mtime_ns, Path(item.path), size, data))
                except OSError:
                    continue  # An entry this user cannot read (another SID's ACL) is left alone.
        stored = sum(item[2] for item in entries)
        total = stored + incoming
        budget = min(self.limit, stored + shutil.disk_usage(self.directory).free - RESERVE_FREE)
        # Entries whose files only the cache still holds cost real disk: drop those first, oldest first.
        for _, path, size, _ in sorted(entries, key=lambda item: (item[3] == 0, item[0])):
            if total <= budget:
                break
            shutil.rmtree(path, ignore_errors=True)
            _ENTRY_SIZES.pop(path.name, None)
            total -= size
        # 2026-11-07: with ~16 000 entries one scan outlasted EVICT_SCAN_SECONDS, so the stamp written at its
        # start had already expired when it finished and the next publish rescanned at once; workers waited on
        # cache.lock for most of the run (test regen: 1.5 rules/min, converters idle at 0 % CPU). The interval
        # counts from the end of a scan.
        if self.limit >= 16 * GIB:
            try:
                stamp.touch()
            except OSError:
                pass
