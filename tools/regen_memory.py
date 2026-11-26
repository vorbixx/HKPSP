from __future__ import annotations

import ctypes
import os
import math
from pathlib import Path

GIB = 1024 ** 3
DEFAULT_PEAK = GIB


def process_peak_bytes():
    if os.name == 'nt':
        from ctypes import wintypes

        class Counters(ctypes.Structure):
            _fields_ = [('cb', wintypes.DWORD), ('page_fault_count', wintypes.DWORD),
                        *[(name, ctypes.c_size_t) for name in
                          ('peak_working_set', 'working_set', 'peak_paged_pool', 'paged_pool',
                           'peak_nonpaged_pool', 'nonpaged_pool', 'pagefile', 'peak_pagefile')]]

        counters = Counters()
        counters.cb = ctypes.sizeof(counters)
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.GetCurrentProcess.restype = wintypes.HANDLE
        psapi = ctypes.WinDLL('psapi', use_last_error=True)
        psapi.GetProcessMemoryInfo.argtypes = (wintypes.HANDLE, ctypes.POINTER(Counters), wintypes.DWORD)
        if not psapi.GetProcessMemoryInfo(kernel.GetCurrentProcess(), ctypes.byref(counters), counters.cb):
            return None
        return int(counters.peak_working_set)
    try:
        import resource
        peak = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
        return int(peak if os.uname().sysname == 'Darwin' else peak * 1024)
    except (ImportError, OSError, AttributeError):
        return None


def estimate(history, name):
    value = history.get(name)
    return max(DEFAULT_PEAK, value) if type(value) is int and 0 < value < 1024 * GIB else DEFAULT_PEAK


def can_admit(available, minimum, history, candidate, running):
    if not running:
        return available >= minimum
    reserve = sum(estimate(history, name) // 2 for name in running)
    return available >= minimum + reserve + estimate(history, candidate)


def memory_gate(root, environ=None):
    """Match validation-lock/heavy-admission's machine RAM or commit floor."""
    root = Path(root)
    main = root
    dot_git = root / '.git'
    if dot_git.is_file():
        pointer = dot_git.read_text(encoding='utf-8-sig').strip()
        if pointer.startswith('gitdir:'):
            git_dir = (root / pointer.split(':', 1)[1].strip()).resolve()
            main = git_dir.parent.parent.parent
    gate_file = main / '_private/write-min-free-gb.txt'
    text = gate_file.read_text(encoding='utf-8-sig').strip() if gate_file.is_file() else ''
    gate = text.splitlines()[0].strip().replace(',', '.') if text else '4'
    commit = gate.startswith('commit:')
    configured = float(gate.split(':', 1)[1] if commit else gate)
    env = os.environ if environ is None else environ
    ram = float(env['HK3DS_WRITE_MIN_FREE_GB']) if env.get('HK3DS_WRITE_MIN_FREE_GB') else (0 if commit else configured)
    commit_floor = configured if commit else 0
    if any(not math.isfinite(v) or v < 0 for v in (ram, commit_floor, configured)):
        raise ValueError('Memory admission floors must be finite and nonnegative')
    return ram * GIB, commit_floor * GIB
