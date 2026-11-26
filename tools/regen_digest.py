"""Conservative persistent SHA-256 memo for regen input fingerprints."""
from __future__ import annotations

import ctypes
from functools import lru_cache
import json
import os
from pathlib import Path
import threading
import time

VERIFY_EVERY = 32
SCHEMA = 2


@lru_cache(maxsize=1)
def windows_api():
    # Bind once: per-file ctypes structure creation costs more than hashing a
    # small JSON file and would defeat memoization for most exported objects.
    class Basic(ctypes.Structure):
        _fields_ = [('CreationTime', ctypes.c_int64), ('LastAccessTime', ctypes.c_int64),
                    ('LastWriteTime', ctypes.c_int64), ('ChangeTime', ctypes.c_int64),
                    ('FileAttributes', ctypes.c_uint32)]
    class Id(ctypes.Structure):
        _fields_ = [('VolumeSerialNumber', ctypes.c_uint64), ('FileId', ctypes.c_ubyte * 16)]
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    kernel.CloseHandle.restype = ctypes.c_int
    create = kernel.CreateFileW
    create.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32,
                       ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
    create.restype = ctypes.c_void_p
    query = kernel.GetFileInformationByHandleEx
    query.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32]
    query.restype = ctypes.c_int
    ioctl = kernel.DeviceIoControl
    ioctl.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p,
                      ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32,
                      ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p]
    ioctl.restype = ctypes.c_int
    return Basic, Id, kernel, create, query, ioctl


def change_token(path: Path):
    """Return identity + content-change token; None means hash without memoization."""
    try:
        if os.name == 'nt':
            # Windows st_ctime is creation time, so use NTFS ChangeTime and FileIdInfo.
            Basic, Id, kernel, create, query, ioctl = windows_api()
            handle = create(str(path), 0x80, 0x1 | 0x2 | 0x4, None, 3, 0x80, None)
            if handle in (None, ctypes.c_void_p(-1).value):
                return None
            try:
                basic, file_id = Basic(), Id()
                if not query(handle, 0, ctypes.byref(basic), ctypes.sizeof(basic)):
                    return None
                if not query(handle, 18, ctypes.byref(file_id), ctypes.sizeof(file_id)):
                    return None
                # ChangeTime can be reset together with LastWriteTime by utime.
                # NTFS USN advances on content writes even when those timestamps
                # are restored. Unsupported filesystems conservatively miss cache.
                output = ctypes.create_string_buffer(4096)
                returned = ctypes.c_uint32()
                if not ioctl(handle, 0x000900eb, None, 0, output, len(output),
                             ctypes.byref(returned), None) or returned.value < 60:
                    return None
                major = ctypes.c_uint16.from_buffer(output, 4).value
                record_size = ctypes.c_uint32.from_buffer(output, 0).value
                if major != 2 or record_size > returned.value or record_size < 60:
                    return None
                usn = ctypes.c_int64.from_buffer(output, 24).value
                if usn <= 0 or not any(file_id.FileId):
                    return None
                size = path.stat().st_size
                return {
                    'file_id': f"{file_id.VolumeSerialNumber}:{bytes(file_id.FileId).hex()}",
                    'size': size,
                    # FILETIME is 100 ns since 1601-01-01; expose Unix mtime_ns.
                    'mtime_ns': (basic.LastWriteTime - 116444736000000000) * 100,
                    'change_time': basic.ChangeTime,
                    'usn': usn,
                }
            finally:
                kernel.CloseHandle(handle)
        stat = path.stat()
        if not stat.st_ino or not stat.st_dev or not hasattr(stat, 'st_ctime_ns'):
            return None
        return {
            'file_id': f'{stat.st_dev}:{stat.st_ino}',
            'size': stat.st_size,
            'mtime_ns': stat.st_mtime_ns,
            'ctime_ns': stat.st_ctime_ns,
        }
    except (OSError, AttributeError, TypeError, ValueError):
        return None


class DigestCache:
    def __init__(self, path: Path, *, verify_all=False):
        self.path = path
        self.verify_all = verify_all
        self.lock = threading.RLock()
        self.records = {}
        self.hash_seconds = 0.0
        self.fingerprint_seconds = 0.0
        self.hash_count = 0
        self.hits = 0
        try:
            state = json.loads(path.read_text(encoding='utf-8'))
            if state.get('schema') == SCHEMA and isinstance(state.get('files'), dict):
                self.records = state['files']
        except (OSError, ValueError, TypeError, AttributeError):
            pass
        # Export-directory digests keyed by the listing signature (names, sizes, mtimes), persisted beside the
        # file memo. 2026-11-01: large directories were hashed in a process pool outside this memo, so every
        # new regen/preview process reread each ~6k-file scene export (planning terrain-scene-Fungus1_03:
        # 31.4 s cold, 3.9 s of it waiting on the pool; only 17 files went through the file memo).
        self.directories_path = path.with_name(path.stem + '.directories.json')
        self.directories = {}
        try:
            state = json.loads(self.directories_path.read_text(encoding='utf-8'))
            if state.get('schema') == SCHEMA and isinstance(state.get('directories'), dict):
                self.directories = state['directories']
        except (OSError, ValueError, TypeError, AttributeError):
            pass

    def directory(self, key, signature):
        """Digests of an export directory whose listing signature is unchanged, or None to rehash.

        Like file records, every VERIFY_EVERY-th reuse rehashes, and verify_all always does."""
        with self.lock:
            record = self.directories.get(key)
            if self.verify_all or not isinstance(record, dict) or record.get('signature') != signature                     or not isinstance(record.get('digests'), dict):
                return None
            try:
                count = int(record.get('uses', 0)) + 1
            except (TypeError, ValueError):
                count = VERIFY_EVERY
            if not count % VERIFY_EVERY:
                return None
            record['uses'] = count
            return record['digests']

    def remember_directory(self, key, signature, digests):
        with self.lock:
            self.directories[key] = {'signature': signature, 'digests': dict(digests), 'uses': 0}

    def digest(self, path: Path, hasher):
        key = path_key(path)
        token = change_token(path)
        with self.lock:
            record = self.records.get(key)
            if (not self.verify_all and token is not None and isinstance(record, dict)
                    and record.get('token') == token
                    and isinstance(record.get('sha256'), str) and len(record['sha256']) == 64
                    and all(c in '0123456789abcdef' for c in record['sha256'])):
                try:
                    count = int(record.get('uses', 0)) + 1
                except (TypeError, ValueError):
                    count = VERIFY_EVERY
                if count % VERIFY_EVERY:
                    record['uses'] = count
                    self.hits += 1
                    return record['sha256']
        start = time.perf_counter()
        result = hasher(path)
        elapsed = time.perf_counter() - start
        after = change_token(path)
        with self.lock:
            self.hash_seconds += elapsed
            self.hash_count += 1
            if token is not None and token == after:
                self.records[key] = {'token': token, 'sha256': result, 'uses': 0}
        return result

    def save(self):
        with self.lock:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            temporary = self.path.with_suffix(self.path.suffix + '.tmp')
            temporary.write_text(json.dumps({'schema': SCHEMA, 'files': self.records},
                                            separators=(',', ':')) + '\n', encoding='utf-8')
            temporary.replace(self.path)
            temporary = self.directories_path.with_suffix(self.directories_path.suffix + '.tmp')
            temporary.write_text(json.dumps({'schema': SCHEMA, 'directories': self.directories},
                                            separators=(',', ':')) + '\n', encoding='utf-8')
            temporary.replace(self.directories_path)


def path_key(path: Path):
    """Return a cheap absolute lexical key; file_id in the token tracks target identity."""
    return os.path.normcase(os.path.abspath(os.fspath(path)))
