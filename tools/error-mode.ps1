# Dot-source and call Disable-ErrorDialogs before starting agent work. Child processes inherit the
# Windows error mode, so a crashing tool exits with its error code instead of opening a modal
# "Application Error" box that waits for a click nobody gives: 2026-10-27 00:30 a crashed dotnet
# patcher waited 40+ min under such a box while holding shared-private.lock, and seven agents queued
# behind it (22:36 python/g++/git start failures waited the same way).
function Disable-ErrorDialogs {
    if (-not ('HK3DS.ErrorMode' -as [type])) {
        Add-Type -Namespace HK3DS -Name ErrorMode -MemberDefinition '
            [System.Runtime.InteropServices.DllImport("kernel32.dll")] public static extern uint SetErrorMode(uint mode);
            [System.Runtime.InteropServices.DllImport("kernel32.dll")] public static extern uint GetErrorMode();'
    }
    # SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX, keeping existing bits.
    [void][HK3DS.ErrorMode]::SetErrorMode([HK3DS.ErrorMode]::GetErrorMode() -bor 0x8003)
    # .NET tools also pop WER UI for unhandled exceptions; the env var disables it for their runtime.
    $env:DOTNET_DbgEnableMiniDump = '0'
}
