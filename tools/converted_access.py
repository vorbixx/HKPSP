"""Process writer leases and exclusive snapshots for a lexical converted root."""
import atexit
from contextlib import contextmanager
import os
from pathlib import Path
import threading


_writers = {}
_mutex = threading.RLock()


if os.name == 'nt':
    import ctypes
    from ctypes import wintypes

    class _Overlapped(ctypes.Structure):
        _fields_ = [('Internal', ctypes.c_size_t), ('InternalHigh', ctypes.c_size_t),
                    ('Offset', wintypes.DWORD), ('OffsetHigh', wintypes.DWORD),
                    ('hEvent', wintypes.HANDLE)]

    _kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    _kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                   wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD,
                                   wintypes.HANDLE]
    _kernel.CreateFileW.restype = wintypes.HANDLE
    _kernel.LockFileEx.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD,
                                  wintypes.DWORD, wintypes.DWORD, ctypes.POINTER(_Overlapped)]
    _kernel.LockFileEx.restype = wintypes.BOOL
    _kernel.UnlockFileEx.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD,
                                    wintypes.DWORD, ctypes.POINTER(_Overlapped)]
    _kernel.UnlockFileEx.restype = wintypes.BOOL
    _kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    _kernel.CloseHandle.restype = wintypes.BOOL


class _Lease:
    def __init__(self, root, exclusive):
        sidecar = Path(root).parent / 'converted-access.lock'
        sidecar.parent.mkdir(parents=True, exist_ok=True)
        if os.name == 'nt':
            # Shared delete access lets temporary checkout fixtures clean up.
            self.handle = _kernel.CreateFileW(str(sidecar), 0xC0000000, 7, None, 4, 0x80, None)
            if self.handle == wintypes.HANDLE(-1).value:
                raise ctypes.WinError(ctypes.get_last_error())
            self.overlapped = _Overlapped()
            if not _kernel.LockFileEx(self.handle, 2 if exclusive else 0, 0, 1, 0,
                                      ctypes.byref(self.overlapped)):
                error = ctypes.WinError(ctypes.get_last_error())
                _kernel.CloseHandle(self.handle)
                raise error
        else:
            import fcntl
            self.handle = os.open(sidecar, os.O_RDWR | os.O_CREAT, 0o666)
            try:
                fcntl.flock(self.handle, fcntl.LOCK_EX if exclusive else fcntl.LOCK_SH)
            except BaseException:
                os.close(self.handle)
                raise

    def close(self):
        if os.name == 'nt':
            _kernel.UnlockFileEx(self.handle, 0, 1, 0, ctypes.byref(self.overlapped))
            _kernel.CloseHandle(self.handle)
        else:
            os.close(self.handle)


def writer_lease(root):
    """Retain one shared lease per converted root until the process exits."""
    root = Path(os.path.abspath(root))
    key = os.path.normcase(str(root))
    with _mutex:
        if key not in _writers:
            _writers[key] = _Lease(root, exclusive=False)


def release_writer_leases():
    """Release retained leases at shutdown; also usable after isolated test fixtures."""
    with _mutex:
        for lease in _writers.values():
            lease.close()
        _writers.clear()


@contextmanager
def exclusive_source(source_converted_root):
    """Wait for writers to exit, then hold a stable source throughout cloning."""
    root = Path(os.path.abspath(source_converted_root))
    with _mutex:
        if os.path.normcase(str(root)) in _writers:
            raise RuntimeError('Prepare converted in a separate process after this writer exits')
    lease = _Lease(root, exclusive=True)
    try:
        yield
    finally:
        lease.close()


atexit.register(release_writer_leases)
