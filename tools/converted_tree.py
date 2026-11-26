"""Share converted files by hardlink; replace directory entries when installing files.

Converters must load converted_files before writing. Cross-volume worktrees use
a locked seed on their volume; unsupported hardlinks still fall back to copies.
"""
from __future__ import annotations

import argparse
import errno
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import uuid

from converted_access import exclusive_source


def install_file(source: Path, destination: Path, *, share=True) -> bool:
    """Atomically install a hardlink (True), or an independent copy (False)."""
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(f'.{destination.name}.share-{uuid.uuid4().hex}.tmp')
    linked = share
    try:
        try:
            if share:
                os.link(source, temporary)
            else:
                shutil.copy2(source, temporary)
        except OSError as error:
            if error.errno not in (errno.EXDEV, errno.EMLINK, errno.EPERM, errno.EACCES,
                                   errno.ENOTSUP, errno.EINVAL):
                raise
            linked = False
            shutil.copy2(source, temporary)
        os.replace(temporary, destination)
    finally:
        temporary.unlink(missing_ok=True)
    return linked


def same_volume(source: Path, destination: Path) -> bool:
    """Compare actual volumes, including paths reached through junctions."""
    return source.stat().st_dev == destination.stat().st_dev


def _reparse(path: Path) -> bool:
    return path.is_symlink() or (hasattr(path, 'is_junction') and path.is_junction())


def _inventory(root: Path):
    if _reparse(root):
        raise ValueError(f'Converted trees must not contain reparse points: {root}')
    directories, files = {Path('.')}, {}
    for current, names, filenames in os.walk(root, followlinks=False):
        current = Path(current)
        for name in names + filenames:
            entry = current / name
            if _reparse(entry):
                raise ValueError(f'Converted trees must not contain reparse points: {entry}')
        directories.update((current / name).relative_to(root) for name in names)
        for name in filenames:
            entry = current / name
            info = entry.stat()
            if not stat.S_ISREG(info.st_mode):
                raise ValueError(f'Not a converted regular file: {entry}')
            files[entry.relative_to(root)] = info
    return directories, files


def _digest(path: Path) -> bytes:
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').digest()


def _refresh_seed(source: Path, seed: Path) -> dict:
    """Caller holds both leases. Every replacement publishes a complete new inode.

    A failed refresh may leave complete files from two generations in the seed,
    but publishes no worktree. The next refresh reconciles all paths again.
    """
    directories, files = _inventory(source)
    seed.mkdir(parents=True, exist_ok=True)
    old_directories, old_files = _inventory(seed)
    # Reconcile deletions and file/directory type changes without touching peers.
    for relative in old_files.keys() - files.keys():
        (seed / relative).unlink()
    for relative in sorted(old_directories - directories, key=lambda p: len(p.parts), reverse=True):
        (seed / relative).rmdir()
    for relative in sorted(directories, key=lambda p: len(p.parts)):
        (seed / relative).mkdir(exist_ok=True)
    result = dict(seedCopiedFiles=0, seedCopiedBytes=0)
    for relative, info in files.items():
        original, cached = source / relative, seed / relative
        old = old_files.get(relative)
        if (old is not None and old.st_size == info.st_size
                and old.st_mtime_ns == info.st_mtime_ns
                and _digest(original) == _digest(cached)):
            continue
        # Never truncate a seed inode: existing worktrees may still link to it.
        install_file(original, cached, share=False)
        result['seedCopiedFiles'] += 1
        result['seedCopiedBytes'] += info.st_size
    return result


def prepare(source: Path, destination: Path, *, seed: Path | None = None) -> dict:
    """Publish a complete new tree; never refresh an existing worktree's outputs."""
    # A writer that opened an unshared inode must finish before we add a peer.
    # Writers in separate checkouts use separate gates and still run in parallel.
    source, destination = source.resolve(), destination.resolve()
    if destination.exists():
        raise FileExistsError(destination)
    if not source.is_dir():
        raise FileNotFoundError(source)
    if destination.is_relative_to(source) or source.is_relative_to(destination):
        raise ValueError('Converted source and destination must be separate trees')
    destination.parent.mkdir(parents=True, exist_ok=True)
    with exclusive_source(source):
        if seed is None or same_volume(source, destination.parent):
            return _prepare(source, destination)
        if _reparse(seed) or _reparse(seed / 'converted'):
            raise ValueError(f'Converted seed must not be a reparse point: {seed}')
        seed = seed.resolve()
        payload = seed / 'converted'
        if (payload.is_relative_to(source) or source.is_relative_to(seed)
                or destination.is_relative_to(seed) or seed.is_relative_to(destination)):
            raise ValueError('Converted seed, source and destination must be separate trees')
        if not same_volume(seed.parent, destination.parent):
            raise ValueError('Converted seed must be on the destination volume')
        # Separate from source leases: even preparations from different checkouts
        # must serialize refresh + linking against the same per-volume seed.
        with exclusive_source(payload):
            refreshed = _refresh_seed(source, payload)
            result = _prepare(payload, destination)
            result.update(refreshed, seed=str(seed))
            return result


def _prepare(source: Path, destination: Path) -> dict:
    source, destination = source.resolve(), destination.resolve()
    if destination.exists():
        raise FileExistsError(destination)
    if destination.is_relative_to(source) or source.is_relative_to(destination):
        raise ValueError('Converted source and destination must be separate trees')
    destination.parent.mkdir(parents=True, exist_ok=True)
    stage = destination.with_name(f'.converted-preparing-{uuid.uuid4().hex}')
    stage.mkdir()
    result = dict(files=0, linkedFiles=0, logicalBytes=0, copiedBytes=0)
    try:
        if not source.is_dir():
            raise FileNotFoundError(source)
        for current, directories, files in os.walk(source, followlinks=False):
            current = Path(current)
            relative = current.relative_to(source)
            (stage / relative).mkdir(exist_ok=True)
            for name in directories + files:
                entry = current / name
                if entry.is_symlink() or (hasattr(entry, 'is_junction') and entry.is_junction()):
                    raise ValueError(f'Converted trees must not contain reparse points: {entry}')
            for name in files:
                entry = current / name
                info = entry.stat()
                if not stat.S_ISREG(info.st_mode):
                    raise ValueError(f'Not a converted regular file: {entry}')
                linked = install_file(entry, stage / relative / name)
                result['files'] += 1
                result['linkedFiles'] += int(linked)
                result['logicalBytes'] += info.st_size
                result['copiedBytes'] += 0 if linked else info.st_size
        stage.rename(destination)
    finally:
        if stage.exists():
            shutil.rmtree(stage)
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--destination', type=Path, required=True)
    parser.add_argument('--seed', type=Path, help='Destination-volume seed (used only across volumes)')
    arguments = parser.parse_args()
    print(json.dumps(prepare(arguments.source, arguments.destination, seed=arguments.seed)))
