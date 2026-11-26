"""Content-addressed private asset graph. Never starts AssetRipper or the game."""
from __future__ import annotations

import argparse
import ast
import ctypes
import fnmatch
from concurrent.futures import FIRST_COMPLETED, ThreadPoolExecutor, wait
from dataclasses import asdict, dataclass, replace
from functools import lru_cache
import hashlib
import json
import os
from pathlib import Path
import re
import runpy
import shutil
import subprocess
import sys
import time

from regen_cache import OutputCache, graph_lock, relative_file
from regen_digest import DigestCache
from regen_memory import can_admit
from regen_retry import retryable_worker_log, run_with_retries

ROOT = Path(__file__).resolve().parents[1]
SCHEMA = 1
HASH_WORKERS = 4
HASH_BUFFER_SIZE = 64 * 1024
INPUT_CHANGE_RETRIES = 3


class InputsChanged(ValueError):
    pass


class CacheMiss(Exception):
    """A restore-lane rule found no usable cache entry; it goes back to the converter queue."""


# 2026-11-04: in a full regen a restored rule (bretta-quest: 1.2 s of work) showed 403 s because it waited
# for one of the converter slots held by 4-minute scene conversions. Rules with a cache entry for their
# identity restore on this separate lane, without a converter slot or a memory admission.
RESTORE_WORKERS = 4


@dataclass(frozen=True)
class Rule:
    name: str
    commands: tuple[tuple[str, ...], ...] = ()
    inputs: tuple[str, ...] = ()
    outputs: tuple[str, ...] = ()
    after: tuple[str, ...] = ()
    copies: tuple[tuple[str, str], ...] = ()
    # Dedicated, flat scratch directories; only these are cleared before a run.
    scratch: tuple[str, ...] = ()
    modules: tuple[str, ...] = ()
    # Audited staging paths may disappear before publication completes.
    temporary_outputs: tuple[str, ...] = ()
    # Frozen graph catalog, used to derive only this room's relevant spellings.
    gate_scenes: tuple[str, ...] = ()
    gate_source: str = ''
    # These declared inputs still fingerprint absence and audit later file reads.
    optional_inputs: tuple[str, ...] = ()


def digest(path: Path, buffer: bytearray | None = None) -> str:
    # Reuse one buffer per hashing worker, including across many tiny files.
    view = memoryview(buffer if buffer is not None else bytearray(HASH_BUFFER_SIZE))
    value = hashlib.sha256()
    with path.open('rb', buffering=0) as stream:
        while size := stream.readinto(view):
            value.update(view[:size])
    return value.hexdigest()


# Scheduling, caching and copy-on-write guards decide when and where a converter runs, never which
# bytes it writes. 2026-10-30 two edits of regen.py alone reran all 351 scenes (5-6 h each), because
# convert_scenes imports regen and every room fingerprinted it as compiler code.
INFRASTRUCTURE_MODULES = frozenset('tools/' + name for name in (
    'regen.py', 'regen_cache.py', 'regen_digest.py', 'regen_retry.py', 'regen_worker.py',
    'converted_access.py', 'converted_files.py', 'converted_tree.py',
    # Serves byte-identical export reads (tests/export_pack_tests.py); 2026-11-01 its import from
    # converted_files made all 773 rules "changed inputs".
    'export_pack.py'))


def compiler_modules(root: Path, scripts: tuple[str, ...]) -> set[str]:
    """Local modules that can change a converter's output: its imports minus the infrastructure."""
    return local_modules(root, scripts) - INFRASTRUCTURE_MODULES


@lru_cache(maxsize=512)
def import_names(source: str) -> tuple[str, ...]:
    """Share parsing across rule workers, keyed by exact text so edits cannot go stale."""
    names = []
    for node in ast.walk(ast.parse(source)):
        if isinstance(node, ast.Import):
            names.extend(alias.name for alias in node.names)
        elif isinstance(node, ast.ImportFrom) and node.module:
            names.append(node.module)
    return tuple(names)


def local_modules(root: Path, scripts: tuple[str, ...]) -> set[str]:
    """Include transitive local imports, including imports inside functions."""
    found = set()
    pending = list(scripts)
    while pending:
        relative = pending.pop()
        if relative in found:
            continue
        found.add(relative)
        path = root / relative
        if not path.is_file() or path.suffix != '.py':
            continue
        for name in import_names(path.read_text(encoding='utf-8-sig')):
            candidate = path.parent / (name.split('.')[0] + '.py')
            if candidate.is_file():
                pending.append(candidate.relative_to(root).as_posix())
    return found


def walk_directory(target: Path):
    """Files of a declared export directory plus a listing signature (names, sizes, mtimes).

    Directory entries carry size and mtime on Windows, so the signature opens no file."""
    if '__pycache__' in target.parts:
        return [], ''
    pending, entries_found = [str(target)], []
    while pending:
        with os.scandir(pending.pop()) as entries:
            for entry in entries:
                if entry.name == '__pycache__':
                    continue
                if entry.is_dir(follow_symlinks=False):
                    pending.append(entry.path)
                elif entry.is_file():
                    stat = entry.stat()
                    entries_found.append((entry.path, stat.st_size, stat.st_mtime_ns))
    entries_found.sort()
    signature = hashlib.sha256('\n'.join(f'{p}\t{s}\t{m}' for p, s, m in entries_found).encode()).hexdigest()
    return [Path(p) for p, _, _ in entries_found], signature


def expand(root: Path, pattern: str) -> list[Path]:
    path = Path(pattern)
    if path.is_absolute():
        if path.is_dir():
            return walk_directory(path)[0]
        return [path] if path.is_file() else []
    target = root / pattern
    if target.is_dir():
        # Only explicitly declared export directories are walked, never _private/.
        return walk_directory(target)[0]
    return sorted(p for p in root.glob(pattern) if p.is_file())


# 2026-10-31: each scene's ~10k-file export directory was fingerprinted four times per run (terrain and
# scene rules, before and after conversion), and every file's change token opens a handle: one core
# for ~1.5 h while 7 of 8 workers waited. Within one process, a directory whose listing (names, sizes,
# mtimes) is unchanged reuses its digests. Saved exports are rewritten through new files, which
# changes the listing; a same-size, same-mtime in-place rewrite during one run is not detected.
_DIRECTORY_DIGESTS = {}
# A first listing still digests every file. Threads share one interpreter, so the coordinator stayed at
# about one core with 1-2 of 8 workers busy (2026-10-31); large directories hash in worker processes.
# The values are plain content SHA-256, the same the digest cache stores.
LARGE_DIRECTORY = 256
_HASH_POOL = None


def _hash_pool():
    global _HASH_POOL
    if _HASH_POOL is None:
        from concurrent.futures import ProcessPoolExecutor
        _HASH_POOL = ProcessPoolExecutor(max_workers=max(2, min(8, (os.cpu_count() or 2) // 2)))
    return _HASH_POOL


def hash_files(paths):
    """Process-pool task: content digests of one directory's files, in order."""
    buffer = bytearray(HASH_BUFFER_SIZE)
    return [digest(Path(path), buffer) for path in paths]


def digest_group(items, cache=None):
    buffer = bytearray(HASH_BUFFER_SIZE)
    hasher = lambda path: digest(path, buffer)
    return {name: cache.digest(path, hasher) if cache else hasher(path) for name, path in items}


def fingerprint(root: Path, patterns, *, digest_cache=None) -> dict[str, str]:
    started = time.perf_counter()
    result, files, directories, pooled = {}, {}, [], []
    for pattern in sorted(set(patterns)):
        target = None if Path(pattern).is_absolute() else root / pattern
        if target is not None and target.is_dir():
            paths, signature = walk_directory(target)
            key = os.path.normcase(str(target))
            names = [p.relative_to(root).as_posix() for p in paths]
            memo = _DIRECTORY_DIGESTS.get(key)
            if memo and memo[0] == signature:
                for name in names:
                    result[name] = memo[1][name]
                continue
            # Across processes the persisted directory memo applies the same listing check.
            saved = digest_cache.directory(key, signature) if digest_cache is not None else None
            if saved is not None and all(name in saved for name in names):
                for name in names:
                    result[name] = saved[name]
                _DIRECTORY_DIGESTS[key] = (signature, {name: saved[name] for name in names})
                continue
            directories.append((key, signature, names))
            if len(paths) >= LARGE_DIRECTORY:
                for name in names:
                    result[name] = ''  # Preserve deterministic pattern/path insertion order.
                pooled.append((names, _hash_pool().submit(hash_files, [str(p) for p in paths])))
                continue
        else:
            paths = expand(root, pattern)
        if not paths:
            result['missing:' + pattern] = ''
        for path in paths:
            name = path.relative_to(root).as_posix() if path.is_relative_to(root) else str(path)
            files[name] = path
            result[name] = ''  # Preserve deterministic pattern/path insertion order.
    # Recheck identity/change tokens on every call, including post-converter
    # verification. Without a reliable token the memo always rereads the bytes.
    items = list(files.items())
    if len(items) < 256:
        if items:  # Every directory may have come from the listing memo.
            result.update(digest_group(items, digest_cache))
    else:
        size = (len(items) + HASH_WORKERS - 1) // HASH_WORKERS
        groups = [items[start:start + size] for start in range(0, len(items), size)]
        # Only four tasks, buffers (256 KiB total), and open files, not one future
        # per file. Collect in input order regardless of worker completion order.
        with ThreadPoolExecutor(max_workers=HASH_WORKERS) as pool:
            for hashes in pool.map(lambda group: digest_group(group, digest_cache), groups):
                result.update(hashes)
    for names, future in pooled:
        result.update(zip(names, future.result()))
    for key, signature, names in directories:
        _DIRECTORY_DIGESTS[key] = (signature, {name: result[name] for name in names})
        if digest_cache is not None:
            digest_cache.remember_directory(key, signature, _DIRECTORY_DIGESTS[key][1])
    if digest_cache is not None:
        with digest_cache.lock:
            digest_cache.fingerprint_seconds += time.perf_counter() - started
    return result


def ordered(rules: list[Rule]) -> list[Rule]:
    by_name = {r.name: r for r in rules}
    if len(by_name) != len(rules):
        raise ValueError('Duplicate asset rule')
    result, visiting, visited = [], set(), set()

    def visit(name):
        if name in visiting:
            raise ValueError('Asset dependency cycle: ' + name)
        if name in visited:
            return
        if name not in by_name:
            raise ValueError('Unknown asset dependency: ' + name)
        visiting.add(name)
        for dependency in by_name[name].after:
            visit(dependency)
        visiting.remove(name)
        visited.add(name)
        result.append(by_name[name])

    for rule in rules:
        visit(rule.name)
    return result


def selected_rules(rules: list[Rule], only: str | None = None) -> list[Rule]:
    """Select rule names by regex, including every transitive prerequisite."""
    graph = ordered(rules)
    if only is None:
        return graph
    if not only:
        raise ValueError('--only requires a nonempty rule-name regex')
    try:
        pattern = re.compile(only)
    except re.error as error:
        raise ValueError(f'Invalid --only regex: {error}') from error
    names = {rule.name for rule in graph if pattern.search(rule.name)}
    if not names:
        raise ValueError(f'No asset rules match --only {only!r}')
    # Reverse topological order expands each prerequisite before its own turn.
    for rule in reversed(graph):
        if rule.name in names:
            names.update(rule.after)
    return [rule for rule in graph if rule.name in names]


def copy_pairs(root: Path, rule: Rule):
    for pattern, directory in rule.copies:
        sources = expand(root, pattern)
        if not sources:
            raise ValueError(f'{rule.name}: no install inputs match {pattern}')
        for source in sources:
            yield source, root / directory / source.name


def output_hashes(root: Path, rule: Rule, digest_cache=None):
    patterns = list(rule.outputs)
    patterns.extend(destination.relative_to(root).as_posix() for _, destination in copy_pairs(root, rule))
    return fingerprint(root, patterns, digest_cache=digest_cache)


def validate_accesses(rule, inputs, accesses, root=None):
    import posixpath

    existing = {p.relative_to(root).as_posix() for pattern in rule.outputs for p in expand(root, pattern)} if root else set()

    def declared_input(path):
        # Importers probe optional metadata and sometimes use paths with '..'.
        # The declared glob permits an absent probe, while a file that appears
        # later must be present in the current fingerprint to be accepted.
        path = posixpath.normpath(path)
        if not relative_file(path):
            return False
        if path in inputs:
            return True
        if not any(declared_file(path, pattern) for pattern in rule.inputs):
            return False
        # Access-audit unit tests may not have a checkout root; in that mode
        # the declared glob is the only evidence available for an optional probe.
        return root is None or not (root / path).exists()

    def output(path):
        return path in existing or any(declared_file(path, pattern)
                                       for pattern in (*rule.outputs, *rule.temporary_outputs))

    # Scene rules freeze their source path and relevant room spellings in the
    # command, so the room-admission table is no longer a byte input to them.
    catalog_modules = {'tools/eastern_rooms.py'} if rule.gate_source else set()
    def declared_absent_read(path):
        # Imports probe optional metadata. A declared directory fingerprints its
        # contents, so a later file appearing there invalidates the cached rule.
        return (root is not None and not (root / path).exists()
                and any((root / pattern).is_dir() and declared_file(path, pattern)
                        for pattern in rule.inputs))

    undeclared_reads = {p for a in accesses for p in a['reads']
                        if not declared_input(p) and p not in INFRASTRUCTURE_MODULES
                        and p not in catalog_modules and not output(p)
                        and not declared_absent_read(p)}
    undeclared_writes = {p for a in accesses for p in a['writes'] if not output(p)}
    if undeclared_reads or undeclared_writes:
        raise ValueError(f'{rule.name}: undeclared converter accesses; add to regen_manifest.py: '
                         f'inputs={sorted(undeclared_reads)[:6]} ({len(undeclared_reads)} total), '
                         f'outputs={sorted(undeclared_writes)[:6]} ({len(undeclared_writes)} total); '
                         'full accesses are saved alongside the converter log')


def read_stamp(path: Path):
    value = {'schema': SCHEMA, 'rules': {}}
    try:
        loaded = json.loads(path.read_text(encoding='utf-8'))
        if loaded.get('schema') == SCHEMA and isinstance(loaded.get('rules'), dict):
            value = loaded
    except (OSError, ValueError, TypeError, AttributeError):
        pass
    # Replay rule changes recorded after the last full save (see StampSaver); a torn last line is ignored.
    try:
        with stamp_journal(path).open(encoding='utf-8') as journal:
            for line in journal:
                try:
                    change = json.loads(line)
                except ValueError:
                    break
                if 'last_runs' in change:
                    value['last_runs'] = change['last_runs']
                elif change.get('record') is None:
                    value['rules'].pop(change.get('name'), None)
                else:
                    value['rules'][change['name']] = change['record']
    except OSError:
        pass
    return value


def stamp_journal(path: Path) -> Path:
    return path.with_name(path.name + '.journal')


class StampSaver:
    """Durable rule state without rewriting the whole stamp for every rule.

    2026-10-31: stamps.json reached 469 MB and was rewritten before and after every rule (~5 s each,
    ~2 h of coordinator time and ~700 GB written per full regen). Each save now appends only the rules
    whose record changed (object identity) to a journal; the full stamp is rewritten at most once a
    minute and at the end, and read_stamp replays the journal after a crash."""

    def __init__(self, path: Path, state, interval=60.0):
        self.path, self.state, self.interval = path, state, interval
        self.seen = dict(state['rules'])
        self.last_runs = state.get('last_runs')
        self.last_full = time.monotonic()

    def save(self, force=False):
        rules = self.state['rules']
        changed = [name for name in set(self.seen) | set(rules) if rules.get(name) is not self.seen.get(name)]
        runs = self.state.get('last_runs')
        if force or time.monotonic() - self.last_full >= self.interval:
            save_json(self.path, self.state)
            stamp_journal(self.path).unlink(missing_ok=True)
            self.last_full = time.monotonic()
        elif changed or runs != self.last_runs:
            lines = [json.dumps({'name': name, 'record': rules.get(name)}, sort_keys=True) for name in sorted(changed)]
            if runs != self.last_runs:
                lines.append(json.dumps({'last_runs': runs}, sort_keys=True))
            self.path.parent.mkdir(parents=True, exist_ok=True)
            with stamp_journal(self.path).open('a', encoding='utf-8') as journal:
                journal.write('\n'.join(lines) + '\n')
                journal.flush()
                os.fsync(journal.fileno())
        self.seen = dict(rules)
        self.last_runs = dict(runs) if isinstance(runs, dict) else runs


def valid_record(record):
    return (isinstance(record, dict) and isinstance(record.get('signature'), str)
            and all(isinstance(record.get(key), dict) for key in ('inputs', 'outputs', 'dependencies'))
            and isinstance(record.get('installed'), list)
            and all(isinstance(p, str) for p in record['installed']))


def save_json(path: Path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    temporary.replace(path)


def declared_file(name, pattern):
    """Anchored glob match, including the zero-directory case of **/*."""
    def match(parts, patterns):
        if not patterns:
            return not parts
        if patterns[0] == '**':
            return match(parts, patterns[1:]) or bool(parts) and match(parts[1:], patterns)
        return bool(parts) and fnmatch.fnmatchcase(parts[0], patterns[0]) and match(parts[1:], patterns[1:])
    return match(Path(name).parts, Path(pattern).parts) or (
        not any(c in pattern for c in '*?[') and Path(name).is_relative_to(Path(pattern)))


def cacheable_record(root, rule, identity, record):
    if not valid_record(record) or any(record.get(k) != v for k, v in identity.items()):
        return False
    outputs = record['outputs']
    if any(not relative_file(name) or not isinstance(value, str) or len(value) != 64
           or not (root / name).resolve().is_relative_to(root.resolve()) for name, value in outputs.items()):
        return False
    installed = {str(Path(directory) / Path(name).name).replace('\\', '/')
                 for pattern, directory in rule.copies for name in (*outputs, *identity['inputs'])
                 if declared_file(name, pattern)}
    return (set(record['installed']) == installed and installed <= outputs.keys()
            and all(name in installed or any(declared_file(name, pattern) for pattern in rule.outputs)
                    for name in outputs))


def retire_installed(root, previous, installed):
    # Retain ownership across a failed run, including a failed cache restore.
    for relative in set(previous.get('installed', [])) - set(installed):
        path = root / relative
        if not path.resolve().is_relative_to((root / '_private/converted').resolve()):
            raise ValueError('Installed output must stay under _private/converted')
        path.unlink(missing_ok=True)


def regen_jobs(jobs=None):
    value = os.environ.get('HK3DS_REGEN_JOBS') if jobs is None else jobs
    if value is None:
        return max(1, min(32, (os.cpu_count() or 1) // 2))
    try:
        workers = int(value)
    except (TypeError, ValueError):
        raise ValueError('HK3DS_REGEN_JOBS must be a positive integer') from None
    if workers < 1:
        raise ValueError('HK3DS_REGEN_JOBS must be a positive integer')
    return workers


def machine_memory_gate(root):
    """Read the main checkout's physical/commit floor; the environment wins."""
    setting = os.environ.get('HK3DS_WRITE_MIN_FREE_GB')
    overridden = bool(setting)
    if not setting:
        main = Path(root)
        git_file = main / '.git'
        if git_file.is_file():
            git_dir = Path(git_file.read_text(encoding='utf-8-sig').split(':', 1)[1].strip())
            main = (main / git_dir).resolve().parents[2]
        try:
            setting = (main / '_private/write-min-free-gb.txt').read_text(encoding='utf-8-sig').splitlines()[0].strip()
        except (OSError, IndexError):
            setting = '4'
    setting = setting.replace(',', '.')
    kind = 'commit' if setting.startswith('commit:') else 'physical'
    try:
        floor = float(setting.removeprefix('commit:')) * 1024 ** 3
    except ValueError:
        if overridden:
            raise ValueError('HK3DS_WRITE_MIN_FREE_GB must be a physical or commit:N floor') from None
        kind, floor = 'physical', 4 * 1024 ** 3
    if not 0 <= floor < float('inf'):
        raise ValueError('memory admission floor must be finite and nonnegative')
    return kind, floor


def memory_gate(root=ROOT):
    """Compatibility API for callers needing a boolean commit-mode flag."""
    kind, floor = machine_memory_gate(root)
    return kind == 'commit', floor


def available_memory_bytes(kind='physical'):
    """Native physical/commit headroom, matching validation-lock without WMI."""
    if isinstance(kind, bool):
        kind = 'commit' if kind else 'physical'
    if os.name == 'nt':
        class MemoryStatus(ctypes.Structure):
            _fields_ = [('length', ctypes.c_ulong), ('load', ctypes.c_ulong),
                        *[(name, ctypes.c_ulonglong) for name in
                          ('total_physical', 'available_physical', 'total_pagefile', 'available_pagefile',
                           'total_virtual', 'available_virtual', 'available_extended_virtual')]]
        status = MemoryStatus()
        status.length = ctypes.sizeof(status)
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        if not kernel.GlobalMemoryStatusEx(ctypes.byref(status)):
            raise ctypes.WinError(ctypes.get_last_error())
        return status.available_pagefile if kind == 'commit' else status.available_physical
    return os.sysconf('SC_AVPHYS_PAGES') * os.sysconf('SC_PAGE_SIZE')



def memory_admission(root):
    """Use the same machine-local gate as validation-lock/heavy-admission."""
    value = os.environ.get('HK3DS_WRITE_MIN_FREE_GB')
    if value is None:
        main = root
        git_file = root / '.git'
        if git_file.is_file():
            git_dir = git_file.read_text(encoding='utf-8').strip().removeprefix('gitdir:').strip()
            main = (root / git_dir).resolve().parents[2]
        gate = main / '_private/write-min-free-gb.txt'
        value = gate.read_text(encoding='utf-8-sig').strip().replace(',', '.') if gate.is_file() else '4'
    commit = value.startswith('commit:')
    minimum = float(value.removeprefix('commit:')) * 1024 ** 3
    if not 0 <= minimum < float('inf'):
        raise ValueError('HK3DS_WRITE_MIN_FREE_GB must be finite and nonnegative')
    return minimum, commit


def write_prefixes(rule):
    """Conservative write patterns include future pages and retired copy targets."""
    patterns = [*rule.outputs, *rule.scratch, *rule.temporary_outputs]
    for source, destination in rule.copies:
        basename = Path(source).name
        patterns.append(destination + '/' + (basename if any(c in basename for c in '*?[') else '*'))
    return tuple(pattern.replace('\\', '/').lower() for pattern in patterns)


@lru_cache(maxsize=65536)
def _segments_may_match(left, right):
    if left == right:
        return True
    left_wild, right_wild = (any(c in part for c in '*?[') for part in (left, right))
    if not left_wild and not right_wild:
        return left == right
    if not left_wild or not right_wild:
        literal, pattern = (left, right) if right_wild else (right, left)
        return fnmatch.fnmatchcase(literal, pattern)
    stems = [re.split(r'[*?\[]', part, maxsplit=1)[0] for part in (left, right)]
    return stems[0].startswith(stems[1]) or stems[1].startswith(stems[0])


def _patterns_overlap(left, right):
    # Compare segment by segment: before 2026-10-30 a wildcard truncated the whole pattern, so every
    # scene rule's 'romfs/.scene-*/<scene>/*' temporary collided and 351 scenes ran one at a time.
    # A segment that differs for certain separates the writes; anything else (including a shorter
    # pattern, which may name a directory holding the longer one) overlaps.
    for a, b in zip(left.split('/'), right.split('/')):
        if a == '**' or b == '**':
            return True
        if not _segments_may_match(a, b):
            return False
    return True


def overlapping_writes(left, right):
    return any(_patterns_overlap(a, b) for a in left for b in right)


def refresh_gate_rule(root, rule):
    if not rule.gate_source:
        return rule
    from scene_sources import gate_catalog
    command = rule.commands[0]
    start = command.index('--supported-scenes') + 1
    end = next((i for i in range(start, len(command)) if command[i].startswith('--')), len(command))
    catalog = gate_catalog(root / rule.gate_source, rule.gate_scenes)
    return replace(rule, commands=(command[:start] + catalog + command[end:], *rule.commands[1:]))


def prepare_rule(root, rule, state, original, changed, digest_cache):
    rule = refresh_gate_rule(root, rule)
    if not set(rule.optional_inputs).issubset(rule.inputs):
        raise ValueError(f'{rule.name}: optional inputs must also be declared inputs')
    scripts = tuple(a for command in rule.commands for a in command if a.endswith('.py')) + rule.modules
    code = compiler_modules(root, scripts)
    if rule.gate_source:
        code.discard('tools/eastern_rooms.py')
    inputs = fingerprint(root, (*rule.inputs, *code), digest_cache=digest_cache)
    if refresh_gate_rule(root, rule).commands != rule.commands:
        raise InputsChanged(f'{rule.name}: gate catalog changed during fingerprinting')
    definition = asdict(rule)
    if not rule.optional_inputs:
        definition.pop('optional_inputs')
    definition.pop('gate_scenes')
    definition.pop('gate_source')
    if not rule.temporary_outputs:
        definition.pop("temporary_outputs")
    signature = hashlib.sha256(json.dumps(dict(rule=definition, python=sys.version,
                                               executable=sys.executable), sort_keys=True).encode()).hexdigest()
    dependencies = {name: hashlib.sha256(json.dumps(state['rules'].get(name), sort_keys=True).encode()).hexdigest()
                    for name in rule.after}
    previous = original.get(rule.name, {})
    if not valid_record(previous):
        previous = {}
    reason = None
    if previous.get('signature') != signature:
        reason = 'new rule or changed command'
    elif previous.get('inputs') != inputs:
        # Name a few changed files: 2026-11-01 a 657-rule regen said only "changed inputs" for 576 rules,
        # so what triggered the fan-out could not be found afterwards.
        old = previous.get('inputs') if isinstance(previous.get('inputs'), dict) else {}
        new = inputs if isinstance(inputs, dict) else {}
        diff = sorted(name for name in set(old) | set(new) if old.get(name) != new.get(name))
        reason = 'changed inputs' + (f" ({len(diff)}: {', '.join(diff[:3])})" if diff else '')
    elif previous.get('dependencies') != dependencies:
        reason = 'changed dependency generation'
    elif any(d in changed for d in rule.after):
        reason = 'regenerated dependency'
    else:
        try:
            if previous.get('outputs') != output_hashes(root, rule, digest_cache):
                reason = 'missing or changed outputs'
        except ValueError:
            reason = 'missing outputs'
    entry = dict(name=rule.name, action='run' if reason else 'skip', reason=reason,
                 conversionSeconds=0.0)
    return entry, code, dict(signature=signature, inputs=inputs, dependencies=dependencies), previous, rule


def cache_eligible(rule):
    # Only Python commands have the existing access audit. Copy-only rules
    # are safe when every source is declared as an input or an output.
    return all(command and command[0] == '{python}' for command in rule.commands)


def execute_rule(root, rule, stamp, entry, code, identity, previous, cache, digest_cache, messages,
                 restore_only=False):
    """Workers touch only their declared files and return a record to the coordinator."""
    inputs = identity['inputs']
    missing = [k[8:] for k in inputs if k.startswith('missing:') and k[8:] not in rule.optional_inputs]
    if missing:
        raise ValueError(f'{rule.name}: missing inputs {missing}; run the reported external extraction first')
    for relative in rule.scratch:
        directory = root / relative
        if not directory.resolve().is_relative_to((root / '_private/converted/regen').resolve()):
            raise ValueError('Scratch directory must stay under _private/converted/regen')
        directory.mkdir(parents=True, exist_ok=True)
        for path in directory.iterdir():
            if path.is_file():
                path.unlink()
    eligible = cache_eligible(rule)
    restored = None
    if eligible:
        try:
            restored = cache.restore(identity, lambda record: cacheable_record(root, rule, identity, record))
            if restored:
                retire_installed(root, previous, restored['installed'])
                if output_hashes(root, rule, digest_cache) != restored['outputs']:
                    restored = None
        except (OSError, ValueError, TimeoutError) as error:
            restored = None
            messages.append(f'CACHE {rule.name}: miss ({error})')
    if restored:
        if fingerprint(root, (*rule.inputs, *code), digest_cache=digest_cache) != inputs:
            raise InputsChanged(f'{rule.name}: inputs changed during generation')
        entry['action'] = 'restore'
        messages.append(f'CACHE {rule.name}: restored verified outputs')
        return restored
    if restore_only:
        raise CacheMiss(rule.name)
    log = stamp.parent / 'logs' / (rule.name + '.log')
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open('w', encoding='utf-8') as stream:
        accesses = []
        for index, command in enumerate(rule.commands):
            args = [sys.executable if a == '{python}' else a for a in command]
            if command[0] == '{python}':
                script_index = next(i for i, a in enumerate(args) if a.endswith('.py'))
                access_path = log.with_name(f'{rule.name}-{index}-accesses.json')
                args = args[:script_index] + [str(Path(__file__).with_name('regen_worker.py')),
                                             str(root), str(access_path)] + args[script_index:]
            started = time.perf_counter()
            # Converters read saved scene exports through per-scene packs (tools/export_pack.py);
            # HK3DS_EXPORT_PACKS=0 in the caller's environment turns them off.
            environment = dict(os.environ, HK3DS_EXPORT_PACKS=os.environ.get('HK3DS_EXPORT_PACKS', '1'))
            if not cache.enabled:
                environment['HK3DS_REGEN_CACHE'] = '0'
            offset = log.stat().st_size

            def retry_worker_failure(result):
                nonlocal offset
                if not result.returncode:
                    return False
                stream.flush()
                with log.open('rb') as source:
                    source.seek(max(offset, log.stat().st_size - 64 * 1024))
                    text = source.read().decode('utf-8', errors='replace')
                    offset = source.tell()
                return retryable_worker_log(result, text)

            completed = run_with_retries(subprocess.run, args, cwd=root, stdout=stream,
                                         stderr=subprocess.STDOUT, env=environment,
                                         should_retry_result=retry_worker_failure)
            elapsed = time.perf_counter() - started
            entry['conversionSeconds'] += elapsed
            if completed.returncode:
                if fingerprint(root, (*rule.inputs, *code), digest_cache=digest_cache) != inputs:
                    raise InputsChanged(f'{rule.name}: inputs changed during generation')
                raise RuntimeError(f'{rule.name} failed ({completed.returncode}); see {log}')
            if command[0] == '{python}':
                access = json.loads(access_path.read_text(encoding='utf-8'))
                accesses.append(access)
                peak = access.get('peakMemoryBytes')
                if type(peak) is int and peak > 0:
                    entry['peakMemoryBytes'] = max(entry.get('peakMemoryBytes', 0), peak)
    validate_accesses(rule, inputs, accesses, root)
    installed = []
    for source, destination in copy_pairs(root, rule):
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
        installed.append(destination.relative_to(root).as_posix())
    # Remove only formerly installed files owned by this rule, e.g. a retired page.
    retire_installed(root, previous, installed)
    outputs = output_hashes(root, rule, digest_cache)
    if any(k.startswith('missing:') for k in outputs):
        raise ValueError(f'{rule.name}: converter did not produce declared outputs')
    # A source changed during conversion: do not certify an inconsistent generation.
    if fingerprint(root, (*rule.inputs, *code), digest_cache=digest_cache) != inputs:
        raise InputsChanged(f'{rule.name}: inputs changed during generation')
    record = dict(**identity, outputs=outputs, installed=installed)
    if eligible and cacheable_record(root, rule, identity, record):
        try:
            cache.publish(identity, record)
        except (OSError, ValueError, TimeoutError) as error:
            messages.append(f'CACHE {rule.name}: publish skipped ({error})')
    return record


def invalidate_rule(state, graph, name):
    # Keep installed-file ownership across failures, including unstarted descendants.
    affected = {name}
    for rule in graph:
        if rule.name in affected or any(dependency in affected for dependency in rule.after):
            affected.add(rule.name)
            previous = state['rules'].get(rule.name)
            if valid_record(previous):
                state['rules'][rule.name] = dict(previous, signature='')
            else:
                state['rules'].pop(rule.name, None)


def regenerate(root: Path, rules: list[Rule], stamp: Path, *, dry_run=False, report=None,
               no_cache=False, cache_dir=None, verify_digests=False, jobs=None, only=None):
    """Schedule ready rules concurrently; only this coordinator commits atomic stamps."""
    started = time.perf_counter()
    full_graph = ordered(rules)
    graph = selected_rules(full_graph, only)
    workers = regen_jobs(jobs)
    # Fingerprinting is read-only and must not collapse to the converter job
    # limit (usually two on developer worktrees). Keep a bounded separate pool.
    prepare_workers = max(HASH_WORKERS, workers)
    writes = {rule.name: write_prefixes(rule) for rule in graph}
    commit, min_free = memory_gate(root)
    memory_kind = 'commit' if commit else 'physical'

    def free_memory():
        return available_memory_bytes('commit') if memory_kind=='commit' else available_memory_bytes()
    if not 0<=min_free<float('inf'):
        raise ValueError('HK3DS_WRITE_MIN_FREE_GB must be finite and nonnegative')
    state = read_stamp(stamp)
    original = dict(state['rules'])
    stamps = StampSaver(stamp, state)
    cache = OutputCache(root, graph_lock, digest, directory=cache_dir, enabled=not no_cache)
    digest_cache = DigestCache(stamp.parent / 'digests.json', verify_all=verify_digests)
    memory_path = stamp.parent / 'memory-history.json'
    try:
        memory_history = json.loads(memory_path.read_text(encoding='utf-8'))
        if not isinstance(memory_history, dict):
            memory_history = {}
    except (OSError, ValueError):
        memory_history = {}
    changed, done, pending = set(), set(), {rule.name for rule in graph}
    plans, running, preparing, errors, messages = {}, {}, {}, {}, {}
    blocked, retries, started_rules, reported = {}, {}, {}, set()
    restorable, restoring, no_fast_restore = set(), set(), set()

    def converting():
        return sum(1 for name in running if name not in restoring)

    prepared_at, submitted_at = {}, {}
    waiting_said = {}

    def progress(name, status):
        reported.add(name)
        now = time.perf_counter()
        elapsed = now - started_rules.get(name, now)
        # 2026-11-04: cache restores of 13 s of work showed 71-224 s; split the time so the wait is visible.
        phases = ''
        if name in prepared_at:
            phases = f'; prepare {prepared_at[name] - started_rules[name]:.1f}s'
            if name in submitted_at:
                phases += (f', queue {submitted_at[name] - prepared_at[name]:.1f}s'
                           f', run {now - submitted_at[name]:.1f}s')
        print(f'[{len(reported)}/{len(graph)}] {name}: {status} ({elapsed:.2f}s{phases})', flush=True)

    def retry(rule):
        retries[rule.name] = retries.get(rule.name, 0) + 1
        if retries[rule.name] > INPUT_CHANGE_RETRIES:
            pending.discard(rule.name)
            errors[rule.name] = InputsChanged(
                f'{rule.name}: inputs changed during generation; retry limit exceeded')
            progress(rule.name, 'failed')
            return
        plans.pop(rule.name, None)
        pending.add(rule.name)
        print(f'RETRY {rule.name}: inputs changed ({retries[rule.name]}/{INPUT_CHANGE_RETRIES})', flush=True)

    def memory_ready(name):
        return can_admit(free_memory(), min_free, memory_history, name, running)

    try:
        with ThreadPoolExecutor(max_workers=workers) as pool, ThreadPoolExecutor(max_workers=prepare_workers) as prepare_pool, \
                ThreadPoolExecutor(max_workers=RESTORE_WORKERS) as restore_pool:
            while True:
                # Harvest in graph order, never completion order. Successful independent
                # work is retained even when another worker failed.
                for rule in graph:
                    future = preparing.get(rule.name)
                    if future is not None and future.done():
                        del preparing[rule.name]
                        try:
                            plans[rule.name] = future.result()
                            prepared_at[rule.name] = time.perf_counter()
                            plan = plans[rule.name]
                            if (not dry_run and plan[0]['reason'] is not None and rule.name not in no_fast_restore
                                    and cache_eligible(plan[4]) and cache.has_entry(plan[2])):
                                restorable.add(rule.name)
                        except InputsChanged:
                            retry(rule)
                        except Exception as error:
                            errors[rule.name] = error
                            pending.discard(rule.name)
                            invalidate_rule(state, full_graph, rule.name)
                            stamps.save()
                            progress(rule.name, 'failed')
                    future = running.get(rule.name)
                    if future is None or not future.done():
                        continue
                    del running[rule.name]
                    restoring.discard(rule.name)
                    try:
                        record = future.result()
                        peak = plans[rule.name][0].pop('peakMemoryBytes', None)
                        if type(peak) is int and peak > 0:
                            previous_peak = memory_history.get(rule.name)
                            memory_history[rule.name] = max(peak, previous_peak if type(previous_peak) is int else 0)
                            save_json(memory_path, memory_history)
                        state['rules'][rule.name] = record
                        # Keep room sweep run evidence outside content/cache identities.
                        if rule.name.startswith('scene-') or rule.name in ('tutorial', 'room-terrain', 'room-rules', 'install-room'):
                            state.setdefault('last_runs', {})[rule.name] = time.time_ns()
                        stamps.save()
                        done.add(rule.name)
                        original[rule.name] = record
                        progress(rule.name, 'finished')
                    except CacheMiss:
                        # The entry vanished or failed validation: convert it in the ordinary queue.
                        restorable.discard(rule.name)
                        no_fast_restore.add(rule.name)
                        pending.add(rule.name)
                    except InputsChanged:
                        retry(rule)
                    except Exception as error:
                        errors[rule.name] = error
                        progress(rule.name, 'failed')
                        # 2026-10-30 175 scenes failed with empty logs and the run was lost before the
                        # final report: print each reason as it happens.
                        print(f'FAILED {rule.name}: {type(error).__name__}: {str(error)[:400]}', flush=True)
                for rule in graph:
                    if rule.name in pending and any(name in errors or name in blocked for name in rule.after):
                        pending.remove(rule.name)
                        blocked[rule.name] = [name for name in rule.after if name in errors or name in blocked]
                        invalidate_rule(state, full_graph, rule.name)
                        progress(rule.name, 'blocked')
                if not pending and not running and not preparing:
                    if dry_run:
                        break
                    affected = set()
                    completed = [rule for rule in graph if rule.name in done]
                    checked = dict(zip((rule.name for rule in completed), prepare_pool.map(
                        lambda rule: fingerprint(root, (*rule.inputs, *plans[rule.name][1]),
                                                 digest_cache=digest_cache), completed)))
                    for rule in graph:
                        if rule.name not in done:
                            continue
                        plan = plans[rule.name]
                        if any(name in affected for name in rule.after):
                            affected.add(rule.name)
                        elif checked[rule.name] != plan[2]['inputs']:
                            affected.add(rule.name)
                            retry(rule)
                    if not affected:
                        break
                    for rule in graph:
                        if rule.name in affected:
                            done.remove(rule.name)
                            invalidate_rule(state, full_graph, rule.name)
                            if rule.name not in errors and rule.name not in pending:
                                plans.pop(rule.name, None)
                                pending.add(rule.name)
                    stamps.save()
                    continue
                candidate = next((rule for rule in graph if rule.name in pending
                                  and rule.name not in preparing
                                  and all(name in done for name in rule.after)
                                  # 2026-11-04: unchanged rules (23 s to prepare) were reported after 481 s:
                                  # finishing one needs no converter slot, so it must not wait for one.
                                  and (rule.name not in plans or dry_run or plans[rule.name][0]['reason'] is None
                                       or (len(restoring) < RESTORE_WORKERS if rule.name in restorable
                                           else converting() < workers))
                                  and not any(overlapping_writes(writes[rule.name], writes[name])
                                              for name in running)
                                  # Preparations only read. Allow them to overlap each other,
                                  # but never launch a conflicting writer while one is reading.
                                  and (rule.name not in plans or not any(
                                      overlapping_writes(writes[rule.name], writes[name])
                                      for name in preparing))), None)
                memory_blocked = False
                if candidate is not None:
                    rule = candidate
                    try:
                        if rule.name not in plans:
                            started_rules[rule.name] = time.perf_counter()
                            messages.setdefault(rule.name, [])
                            if len(preparing) < prepare_workers:
                                preparing[rule.name] = prepare_pool.submit(
                                    prepare_rule, root, rule, {'rules': dict(state['rules'])},
                                    dict(original), set(changed), digest_cache)
                                continue
                            if preparing:
                                wait(preparing.values(), return_when=FIRST_COMPLETED)
                            continue
                        plan = plans[rule.name]
                        entry = plan[0]
                        if entry['reason'] is None or dry_run:
                            if entry['reason'] is not None:
                                changed.add(rule.name)
                                messages[rule.name].append(f"REGEN {rule.name}: {entry['reason']}")
                            pending.remove(rule.name)
                            done.add(rule.name)
                            progress(rule.name, 'unchanged' if entry['reason'] is None else 'planned')
                            continue
                        if rule.name in restorable:
                            invalidate_rule(state, full_graph, rule.name)
                            stamps.save()
                            changed.add(rule.name)
                            messages[rule.name].append(f"REGEN {rule.name}: {entry['reason']} (cache lane)")
                            restorable.discard(rule.name)
                            restoring.add(rule.name)
                            submitted_at[rule.name] = time.perf_counter()
                            running[rule.name] = restore_pool.submit(
                                execute_rule, root, plan[4], stamp, *plan[:4], cache, digest_cache,
                                messages[rule.name], restore_only=True)
                            pending.remove(rule.name)
                            continue
                        memory_blocked = not memory_ready(rule.name)
                        if not memory_blocked:
                            invalidate_rule(state, full_graph, rule.name)
                            stamps.save()
                            # Large stamp writes can take seconds; other workers
                            # may have consumed RAM since the first admission check.
                            memory_blocked = not memory_ready(rule.name)
                            if not memory_blocked:
                                changed.add(rule.name)
                                messages[rule.name].append(f"REGEN {rule.name}: {entry['reason']}")
                                submitted_at[rule.name] = time.perf_counter()
                                running[rule.name] = pool.submit(execute_rule, root, plan[4], stamp, *plan[:4],
                                                                 cache, digest_cache, messages[rule.name])
                                pending.remove(rule.name)
                                continue
                    except InputsChanged:
                        retry(rule)
                        continue
                    except Exception as error:
                        invalidate_rule(state, full_graph, rule.name)
                        stamps.save()
                        errors[rule.name] = error
                        pending.remove(rule.name)
                        progress(rule.name, 'failed')
                        continue
                if running or preparing:
                    wait((*running.values(), *preparing.values()), timeout=10 if memory_blocked else None,
                         return_when=FIRST_COMPLETED)
                elif memory_blocked:
                    # 2026-11-27: a laptop waited here for an hour with no output.
                    if time.perf_counter() - waiting_said.get('at', -1e9) >= 60:
                        waiting_said['at'] = time.perf_counter()
                        print(f'Waiting for free memory: {free_memory() / 1024 ** 3:.1f} GB free, '
                              f'{min_free / 1024 ** 3:.1f} GB needed before {rule.name}. '
                              'Close other programs or set HK3DS_WRITE_MIN_FREE_GB lower.', flush=True)
                    time.sleep(10)
    finally:
        if not dry_run:
            stamps.save(force=True)
        # Workers write per-rule logs; console output and reports have stable graph order.
        for rule in graph:
            for message in messages.get(rule.name, ()):
                print(message, flush=True)
    if not dry_run:
        digest_cache.save()
    entries = [plans[rule.name][0] if rule.name in plans else
               dict(name=rule.name, action='blocked', reason=str(blocked.get(rule.name)), conversionSeconds=0.0)
               for rule in graph]
    conversion_seconds = sum(entry['conversionSeconds'] for entry in entries)
    result = dict(schema=SCHEMA, dryRun=dry_run, ran=sorted(changed), rules=entries,
                  errors={name: str(error) for name, error in errors.items()}, blocked=blocked,
                  timings=dict(wallSeconds=round(time.perf_counter() - started, 3),
                               hashSeconds=round(digest_cache.hash_seconds, 3),
                               fingerprintSeconds=round(digest_cache.fingerprint_seconds, 3),
                               filesHashed=digest_cache.hash_count, cacheHits=digest_cache.hits,
                               conversionSeconds=round(conversion_seconds, 3)))
    if report:
        save_json(report, result)
    print(f'Asset regeneration: {len(changed)} run, {len(entries) - len(changed)} unchanged', flush=True)
    print(f"Timing: fingerprints {digest_cache.fingerprint_seconds:.3f}s summed across workers; "
          f"hash reads {result['timings']['hashSeconds']:.3f}s summed across workers "
          f"({digest_cache.hash_count} files, {digest_cache.hits} digest-cache hits); "
          f"conversion {conversion_seconds:.3f}s summed across workers", flush=True)
    if errors:
        print(f'Regeneration failures: {len(errors)} failed, {len(blocked)} blocked', flush=True)
        for name, error in errors.items():
            print(f'  {name}: {error}', flush=True)
        for name, dependencies in blocked.items():
            print(f"  {name}: blocked by {', '.join(dependencies)}", flush=True)
        raise next(errors[rule.name] for rule in graph if rule.name in errors)
    return result


def scoped_graph(factory, only):
    """Discover scene prerequisites before reading room actor metadata."""
    if only is None:
        return factory()
    scenes = set()
    while True:
        graph = factory(actor_scenes=scenes)
        needed = {rule.name.removeprefix('scene-') for rule in selected_rules(graph, only)
                  if rule.name.startswith('scene-')}
        if needed <= scenes:
            return graph
        scenes = scenes | needed

# Compatibility name for callers and scoped-input regression guards.
rules_for_scope = scoped_graph


def main():
    # One interpreter for every regen: the PATH Python lacks numpy (scene-* rules failed), and the interpreter
    # is part of each rule's command signature, so a second one would mark every rule changed.
    from toolchain import use_tool_python
    use_tool_python()
    # The graph itself must also bypass timestamp-validated .pyc files.
    namespace = runpy.run_path(str(Path(__file__).with_name('regen_manifest.py')))
    catalog, rules, validate_catalog = (namespace[name] for name in ('catalog', 'rules', 'validate_catalog'))
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('--no-cache', action='store_true', help='Disable shared output and tex3ds caches')
    parser.add_argument('--verify-digests', action='store_true',
                        help='Hash every file; ignore the safe digest memo')
    parser.add_argument('--list', action='store_true', help='Print complete converter/extractor catalog as JSON')
    parser.add_argument('--only', help='Regenerate matching rule names (regex) and their prerequisites')
    parser.add_argument('--psp-route', action='store_true', help='Limit room rules to the PSP first-ending route')
    args = parser.parse_args()
    if args.psp_route:
        from functools import partial
        sys.path.insert(0, str(ROOT / 'tools/psp'))
        from room_packages import census_itinerary
        itinerary = census_itinerary(ROOT / 'tools/psp/route-itinerary.md')
        route = {scene for rows in itinerary.values() for scene, status in rows
                 if status != 'U' and scene != 'Tutorial_01'}
        rules = partial(rules, scene_filter=route)
    graph = scoped_graph(rules, args.only)
    # Tool registration covers all admitted content, even when this build only
    # schedules the PSP route's room rules.
    validate_catalog(ROOT, namespace['rules'](actor_scenes=set()) if args.psp_route else graph)
    selection = selected_rules(graph, args.only)
    if args.list:
        descriptions = []
        for rule in selection:
            scripts = tuple(a for cmd in rule.commands for a in cmd if a.endswith('.py')) + rule.modules
            descriptions.append(dict(**asdict(rule), compilerSources=sorted(compiler_modules(ROOT, scripts))))
        print(json.dumps(dict(rules=descriptions, catalog=catalog()), indent=2))
        return
    external = [item for item in catalog() if item['mode'] == 'external']
    print(f'External extraction: {len(external)} report-only steps; saved exports are reused. '
          'Use --list for inputs/outputs; no AssetRipper or original game is launched.', flush=True)
    build = ROOT / 'build/regen'
    with graph_lock(build / 'regen.lock'):
        save_json(build / 'external-steps.json', external)
        regenerate(ROOT, graph, build / 'stamps.json', dry_run=args.dry_run,
                   # A dry run must not replace the last real run's per-rule conversion times.
                   report=build / ('report-dryrun.json' if args.dry_run else 'report.json'), no_cache=args.no_cache,
                   verify_digests=args.verify_digests, only=args.only)


if __name__ == '__main__':
    sys.modules['regen'] = sys.modules[__name__]
    try:
        main()
    except (OSError, ValueError, RuntimeError) as error:
        print(f'Asset regeneration failed: {error}', file=sys.stderr)
        sys.exit(1)
