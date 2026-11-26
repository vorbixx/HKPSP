"""Copy on write for hardlinked offline conversion outputs.

Import before writing converted assets, including before starting native tools.
The hook is process-wide: pathlib, shutil and image codecs use the same guard.
"""
from contextlib import contextmanager
import builtins
import ctypes
import errno
import io
import os
from pathlib import Path
import shlex
import shutil
import stat
import sys
import tempfile
import threading
import time
from converted_access import writer_lease


_state = threading.local()
_lock = threading.RLock()


def private_output(path, root, *, allow_build=False):
    """Admit lexical private outputs, including this checkout's converted junction."""
    path = Path(path).absolute()
    root = Path(root).absolute()
    private = root / '_private'
    if path.is_relative_to(private):
        resolved = path.resolve()
        if resolved.is_relative_to(private.resolve()):
            return True
        converted = private / 'converted'
        return (path.is_relative_to(converted)
                and resolved.is_relative_to(converted.resolve()))
    build = root / 'build'
    return (allow_build and path.is_relative_to(build)
            and path.resolve().is_relative_to(build.resolve()))


def _replace_shared(source, target, timeout=60.0):
    """Allow transient peer readers to finish; diagnose persistent Windows holders."""
    deadline = time.monotonic() + timeout
    delay = 0.25
    while True:
        try:
            os.replace(source, target)
            return
        except OSError as error:
            if os.name != 'nt' or getattr(error, 'winerror', None) not in (5, 32, 33):
                raise
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                holders = _windows_holders(target)
                error.strerror = f'{error.strerror}; replacement waited {timeout:g}s; {holders}'
                raise
            time.sleep(min(delay, remaining))
            delay = min(delay * 2, 2.0)


def _windows_holders(path):
    """Restart Manager reports processes using a file without requiring WMI."""
    from ctypes import wintypes as w

    class UniqueProcess(ctypes.Structure):
        _fields_ = [('pid', w.DWORD), ('started', w.FILETIME)]

    class ProcessInfo(ctypes.Structure):
        _fields_ = [('process', UniqueProcess), ('app', w.WCHAR * 256),
                    ('service', w.WCHAR * 64), ('kind', ctypes.c_int),
                    ('status', w.ULONG), ('session', w.DWORD), ('restartable', w.BOOL)]

    try:
        rm = ctypes.WinDLL('rstrtmgr', use_last_error=True)
        rm.RmStartSession.argtypes = [ctypes.POINTER(w.DWORD), w.DWORD, w.LPWSTR]
        rm.RmRegisterResources.argtypes = [w.DWORD, w.UINT, ctypes.POINTER(w.LPCWSTR),
                                         w.UINT, ctypes.POINTER(UniqueProcess),
                                         w.UINT, ctypes.POINTER(w.LPCWSTR)]
        rm.RmGetList.argtypes = [w.DWORD, ctypes.POINTER(w.UINT), ctypes.POINTER(w.UINT),
                                ctypes.POINTER(ProcessInfo), ctypes.POINTER(w.DWORD)]
        rm.RmEndSession.argtypes = [w.DWORD]
        for name in ('RmStartSession', 'RmRegisterResources', 'RmGetList', 'RmEndSession'):
            getattr(rm, name).restype = w.DWORD
        session = w.DWORD()
        code = rm.RmStartSession(ctypes.byref(session), 0, ctypes.create_unicode_buffer(33))
        if code:
            return f'Restart Manager unavailable (error {code})'
        try:
            paths = (w.LPCWSTR * 1)(str(Path(path).absolute()))
            code = rm.RmRegisterResources(session, 1, paths, 0, None, 0, None)
            if code:
                return f'Restart Manager registration failed (error {code})'
            needed, count, reasons = w.UINT(), w.UINT(), w.DWORD()
            entries = None
            for _ in range(8):  # The process list can change between calls.
                code = rm.RmGetList(session, ctypes.byref(needed), ctypes.byref(count),
                                    entries, ctypes.byref(reasons))
                if code != 234:  # ERROR_MORE_DATA
                    break
                count.value = needed.value
                entries = (ProcessInfo * count.value)()
            if code:
                return f'Restart Manager query failed (error {code})'
            holders = [f'{entry.app or entry.service or "unknown"} (PID {entry.process.pid})'
                       for entry in (entries or [])[:count.value]]
            return 'Restart Manager holders: ' + (', '.join(holders) or 'none reported; check directory ACLs/read-only attributes')
        finally:
            rm.RmEndSession(session)
    except OSError as error:
        return f'Restart Manager unavailable ({error})'


def _shared_read_descriptor(path, flags):
    """Create a CRT descriptor whose handle permits peer hardlink replacement."""
    import msvcrt
    from ctypes import wintypes as w

    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    create = kernel.CreateFileW
    create.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, w.LPVOID, w.DWORD, w.DWORD, w.HANDLE]
    create.restype = w.HANDLE
    close = kernel.CloseHandle
    close.argtypes = [w.HANDLE]
    close.restype = w.BOOL
    sys.audit('open', path, None, flags)
    handle = create(os.fsdecode(path), 0x80000000, 7, None, 3, 0x80, None)
    if handle == w.HANDLE(-1).value:
        error = ctypes.WinError(ctypes.get_last_error())
        error.filename = os.fsdecode(path)
        raise error
    try:
        return msvcrt.open_osfhandle(handle, flags | os.O_NOINHERIT)
    except BaseException:
        close(handle)
        raise


def _read_open(original):
    def open_shared(file, mode='r', buffering=-1, encoding=None, errors=None,
                    newline=None, closefd=True, opener=None):
        if (opener is None and isinstance(mode, str) and mode in ('r', 'rt', 'rb', 'tr', 'br')
                and _converted_path(file) is not None):
            opener = _shared_read_descriptor
        return original(file, mode, buffering, encoding, errors, newline, closefd, opener)
    return open_shared


def _read_os_open(path, flags, mode=0o777, *, dir_fd=None):
    writing = os.O_WRONLY | os.O_RDWR | os.O_TRUNC | os.O_APPEND | os.O_CREAT | os.O_EXCL
    if dir_fd is None and not flags & writing and _converted_path(path) is not None:
        return _shared_read_descriptor(path, flags)
    return _original_os_open(path, flags, mode, dir_fd=dir_fd)


def _mkdir_converted(path, mode=0o777, *, dir_fd=None):
    # Python 3.12+ gives 0700 directories a protected owner-only Windows DACL.
    # Converter staging directories are renamed into shared sandbox-readable assets.
    if dir_fd is None and mode == 0o700 and _converted_path(path) is not None:
        mode = 0o777
    return _original_mkdir(path, mode, dir_fd=dir_fd)


def is_internal_access():
    """Let the regeneration graph ignore the guard's implementation accesses."""
    return bool(getattr(_state, 'internal', False))


@contextmanager
def internal_access():
    previous = is_internal_access()
    _state.internal = True
    try:
        yield
    finally:
        _state.internal = previous


def _converted_path(value, cwd=None):
    if not isinstance(value, (str, bytes, os.PathLike)):
        return None
    value = os.fsdecode(value)
    if not value or '\0' in value:
        return None
    path = Path(value)
    if not path.is_absolute():
        path = Path(cwd or os.getcwd()) / path
    path = Path(os.path.abspath(path))
    parts = [part.casefold() for part in path.parts]
    if any(parts[i:i + 2] == ['_private', 'converted'] for i in range(len(parts) - 1)):
        return path
    return None


def detach(path, *, cwd=None):
    """Preserve bytes while replacing a shared converted file with a private inode."""
    path = _converted_path(path, cwd)
    if path is None:
        return
    with _lock, internal_access():
        _writer_lease(path)
        try:
            info = path.stat()
        except OSError as error:
            if error.errno in (errno.ENOENT, errno.ENOTDIR, errno.EINVAL, errno.ENAMETOOLONG):
                return
            raise
        if not stat.S_ISREG(info.st_mode) or info.st_nlink < 2:
            return
        descriptor, temporary = tempfile.mkstemp(prefix='.' + path.name + '.cow-', dir=path.parent)
        try:
            with os.fdopen(descriptor, 'wb') as output, path.open('rb') as source:
                shutil.copyfileobj(source, output)
            shutil.copystat(path, temporary)
            _replace_shared(temporary, path)
        finally:
            if os.path.exists(temporary):
                os.unlink(temporary)


def unlink_converted(path):
    """Unlink one converted output without changing a readonly peer hardlink."""
    original = path
    path = _converted_path(path)
    if path is None:
        os.unlink(original)
        return
    with _lock, internal_access():
        _writer_lease(path)
        try:
            info = path.lstat()
        except OSError as error:
            if error.errno in (errno.ENOENT, errno.ENOTDIR, errno.EINVAL, errno.ENAMETOOLONG):
                return
            raise
        is_junction = getattr(path, 'is_junction', None)
        if is_junction and is_junction():
            os.rmdir(path)
            return
        if os.name == 'nt' and stat.S_ISREG(info.st_mode) and not info.st_mode & stat.S_IWRITE:
            _unlink_readonly_windows(path)
            return
        try:
            os.unlink(path)
        except PermissionError:
            info = path.lstat()
            if os.name == 'nt' and stat.S_ISREG(info.st_mode):
                # Writable hardlinks can also deny ordinary deletion under a
                # worktree sandbox. Never chmod a peer's shared file record.
                _unlink_readonly_windows(path)
                return
            if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode) or info.st_nlink > 1:
                raise
            if info.st_nlink > 1:
                raise
            # A single-link file is private to this name, so changing its mode is safe.
            os.chmod(path, info.st_mode | stat.S_IWRITE)
            os.unlink(path)


def _unlink_readonly_windows(path, attempts=40, delay=0.25):
    """Delete this name while leaving the shared file attributes untouched."""
    from ctypes import wintypes

    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    create = kernel.CreateFileW
    create.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                       wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    create.restype = wintypes.HANDLE
    set_info = kernel.SetFileInformationByHandle
    set_info.argtypes = [wintypes.HANDLE, ctypes.c_int, wintypes.LPVOID, wintypes.DWORD]
    set_info.restype = wintypes.BOOL
    close = kernel.CloseHandle
    close.argtypes = [wintypes.HANDLE]
    close.restype = wintypes.BOOL

    class Disposition(ctypes.Structure):
        _fields_ = [('flags', wintypes.DWORD)]

    for attempt in range(attempts):
        handle = create(str(path), 0x00010000, 7, None, 3, 0, None)  # DELETE, share all, OPEN_EXISTING
        if handle == wintypes.HANDLE(-1).value:
            code = ctypes.get_last_error()
        else:
            disposition = Disposition(0x11)  # DELETE | IGNORE_READONLY_ATTRIBUTE
            ok = set_info(handle, 21, ctypes.byref(disposition), ctypes.sizeof(disposition))
            code = 0 if ok else ctypes.get_last_error()
            close(handle)
            if ok:
                return
        if code not in (5, 32, 33) or attempt == attempts - 1:
            raise ctypes.WinError(code)
        time.sleep(delay)


def _writer_lease(path):
    parts = [part.casefold() for part in path.parts]
    index = next(i for i in range(len(parts) - 1)
                 if parts[i:i + 2] == ['_private', 'converted'])
    writer_lease(Path(*path.parts[:index + 2]))


def _mutation(path):
    path = _converted_path(path)
    if path is not None:
        with internal_access():
            _writer_lease(path)


def _audit(event, args):
    if is_internal_access():
        return
    if event == 'open':
        path, mode, flags = args
        if flags & (os.O_WRONLY | os.O_RDWR | os.O_TRUNC | os.O_APPEND):
            # Exclusive creation must retain its normal FileExistsError behavior.
            if not flags & os.O_EXCL:
                detach(path)
            else:
                _mutation(path)
    elif event == 'os.truncate':
        detach(args[0])
    elif event in ('os.remove', 'os.rmdir', 'os.mkdir'):
        _mutation(args[0])
    elif event == 'os.rename':
        _mutation(args[0])
        _mutation(args[1])
    elif event == 'os.link':
        _mutation(args[1])
    elif event == 'subprocess.Popen':
        executable, arguments, cwd, environment = args
        if isinstance(arguments, (str, bytes)):
            arguments = shlex.split(os.fsdecode(arguments), posix=os.name != 'nt')
        # The Python worker installs this hook itself. Its CLI includes input
        # banks, so detaching every path here needlessly replaces shared reads.
        script = next((os.fsdecode(a).strip('"') for a in arguments[1:]
                       if isinstance(a, (str, bytes, os.PathLike)) and os.fsdecode(a).endswith('.py')), None)
        if script and Path(os.fsdecode(executable or arguments[0])).resolve() == Path(sys.executable).resolve():
            worker = Path(script)
            if not worker.is_absolute():
                worker = Path(cwd or os.getcwd()) / worker
            if worker.resolve() == Path(__file__).with_name('regen_worker.py').resolve():
                return
        for argument in arguments:
            if isinstance(argument, (str, bytes, os.PathLike)):
                value = os.fsdecode(argument).strip('"')
                # Compiler references are inputs, not output path arguments.
                if value.casefold().startswith(('-r:', '/r:', '-reference:', '/reference:')):
                    continue
                # Cover compiler outputs, long options and path-bearing switches.
                if value.casefold().startswith(('-out:', '/out:')):
                    value = value[5:].strip('"')
                elif value.startswith('--') and '=' in value:
                    value = value.split('=', 1)[1]
                elif value.startswith('-o') and len(value) > 2:
                    # tex3ds accepts an output path attached directly to -o.
                    value = value[2:]
                elif (value.startswith(('-', '/')) and ':' in value and
                      not (value.startswith('-o') and len(value) > 3 and
                           value[3] == ':' and value[2].isalpha())):
                    option, candidate = value.split(':', 1)
                    is_option = (len(option) > 1 and
                                 all(char.isalnum() or char in '-_' for char in option[1:]))
                    candidate = candidate.strip('"')
                    if is_option and ('/' in candidate or '\\' in candidate):
                        value = candidate
                value = value.strip('"')
                detach(value, cwd=cwd)


if not getattr(sys, '_hk3ds_converted_files_audit_installed', False):
    if os.name == 'nt':
        _original_os_open = os.open
        _original_mkdir = os.mkdir
        builtins.open = _read_open(builtins.open)
        io.open = _read_open(io.open)
        os.open = _read_os_open
        os.mkdir = _mkdir_converted
    sys.addaudithook(_audit)
    sys._hk3ds_converted_files_audit_installed = True

# Saved scene exports are served from per-scene packs when the regeneration graph asks for it
# (tools/export_pack.py); every converter imports this module before reading anything.
if os.environ.get('HK3DS_EXPORT_PACKS') == '1':
    import export_pack  # noqa: E402,F401
