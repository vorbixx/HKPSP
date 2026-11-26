# PPSSPP launcher for HKPSP (functional/visual checks only; PPSSPP timing says nothing about hardware).
#   tools/psp/ppsspp.ps1 -Check                      find PPSSPP, print version and memstick path
#   tools/psp/ppsspp.ps1 -Eboot psp/EBOOT.PBP [-TimeoutSec 60] [-Data <dir>]
#       copies EBOOT (and optional data dir) to <memstick>/PSP/GAME/HKPSP, runs it windowed with
#       --escape-exit and --log, kills it after the timeout, then copies the game's self-capture
#       folder <memstick>/PSP/GAME/HKPSP/out to build/psp/run-<stamp>/ with the emulator log.
#       The run ends when the game writes out/done.txt, the process exits, or the timeout hits.
#   -Perf [-Eboot <perf PBP>] uses HKPSPPERF/out and the sibling HKPSP/data.
#   -Gpu [-Eboot <GPU PBP>] uses HKPSPGPU/out and the sibling HKPSP/data.
#   -Diag [-DiagConfig <dir>] uses HKPSPDIAG/diag and the sibling HKPSP/data.
#   -Psp1000 emulates a PSP-1000 (32 MB, MEMSIZE ignored: the 24 MiB user partition) in its own portable copy;
#       user 2026-11-21: PSP-1000 is supported, so the shipped boot must also pass there.
#   -ExactPixels uses a private portable copy, native 1x software rasterization;
#       required for byte-exact render-target CLUT parity (Vulkan differs).
# Lookup: $env:PPSSPP_EXE, then F:/Tools/PPSSPP, C:/Tools/PPSSPP, C:/Program Files/PPSSPP.
# Memstick: $env:PPSSPP_MEMSTICK, else <exe dir>/memstick (portable zip layout; verify after install,
# docs/psp/ppsspp.md). Flags used are the documented 1.20 ones: --escape-exit, --windowed, --log=FILE.
param(
    [switch]$Check,
    [switch]$Play,
    [switch]$Perf,
    [switch]$Gpu,
    [switch]$Diag,
    [string]$DiagConfig,
    [switch]$ExactPixels,
    [switch]$Psp1000,
    [string]$Eboot,
    [string]$Data,
    [int]$TimeoutSec = 60,
    # -Play smoke check: keep the memstick lock, wait this long, report "play alive: True|False",
    # then stop the emulator. Without it -Play returns at once and releases the lock, so another
    # run's leftover cleanup may stop the game (play smoke "process not found", 2026-11-17).
    [int]$AliveSeconds = 0,
    # Run psp/EBOOT.PBP even though a C++ source is newer (bisecting an old build on purpose).
    [switch]$AllowStaleEboot
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if ($Gpu -and ($Perf -or $Play)) { throw '-Gpu, -Perf and -Play are mutually exclusive' }
if ($Diag -and ($Gpu -or $Perf -or $Play)) { throw '-Diag cannot be combined with -Gpu, -Perf or -Play' }
if ($DiagConfig -and -not $Diag) { throw '-DiagConfig requires -Diag' }

function Find-Ppsspp {
    $candidates = @($env:PPSSPP_EXE) + @('F:/Tools/PPSSPP', 'C:/Tools/PPSSPP', 'C:/Program Files/PPSSPP' |
        ForEach-Object { Join-Path $_ 'PPSSPPWindows64.exe' })
    foreach ($c in $candidates) { if ($c -and (Test-Path $c)) { return (Resolve-Path $c).Path } }
    throw 'PPSSPP not found: set PPSSPP_EXE or install it (docs/psp/ppsspp.md).'
}

$exe = Find-Ppsspp
$memstick = if ($env:PPSSPP_MEMSTICK) { $env:PPSSPP_MEMSTICK } else { Join-Path (Split-Path $exe) 'memstick' }
if ($ExactPixels) {
    # Framebuffer CLUT lookup on Vulkan is not byte-exact, even at 1x. Use the
    # native software rasterizer for parity, in a private portable installation.
    # Never rewrite the user's global PPSSPP settings or memory-stick contents.
    $exact=Join-Path $repo 'build/psp/ppsspp-exact'
    New-Item -ItemType Directory -Force $exact | Out-Null
    $exactExe=Join-Path $exact 'PPSSPPWindows64.exe'
    if ([IO.Path]::GetFullPath($exe) -ne [IO.Path]::GetFullPath($exactExe)) {
        Copy-Item -LiteralPath $exe -Destination $exactExe -Force
    }
    $assets=Join-Path $exact 'assets'
    if (-not (Test-Path -LiteralPath $assets)) {
        Copy-Item -LiteralPath (Join-Path (Split-Path $exe) 'assets') -Destination $assets -Recurse
    }
    $settings=Join-Path $exact 'memstick/PSP/SYSTEM'
    New-Item -ItemType Directory -Force $settings | Out-Null
    $original=Join-Path $memstick 'PSP/SYSTEM/ppsspp.ini'
    $ini=if (Test-Path -LiteralPath $original) { [IO.File]::ReadAllText($original) } else { "[Graphics]`n" }
    $ini=$ini -replace '(?m)^(SoftwareRenderer|InternalResolution)\s*=.*\r?\n',''
    if ($ini -notmatch '(?m)^\[Graphics\]\r?$') { $ini+="`n[Graphics]`n" }
    $ini=$ini -replace '(?m)^\[Graphics\]\r?$',"[Graphics]`nSoftwareRenderer = True`nInternalResolution = 1"
    [IO.File]::WriteAllText((Join-Path $settings 'ppsspp.ini'),$ini,[Text.UTF8Encoding]::new($false))
    $exe=$exactExe;$memstick=Join-Path $exact 'memstick'
}
elseif (-not $Play -and -not $env:PPSSPP_MEMSTICK) {
    # 2026-11-18 (user "how to speed up checks"): every checkout runs checks in its own portable copy
    # (exe + assets ~41 MB, own memstick and save) so agents no longer queue on one shared memstick
    # lock and never restore another run's tutorial.sav. -Play keeps the user's memstick.
    $portable=Join-Path $repo ($(if ($Psp1000) { 'build/psp/ppsspp-portable-1000' } else { 'build/psp/ppsspp-portable' }))
    New-Item -ItemType Directory -Force $portable | Out-Null
    $portableExe=Join-Path $portable 'PPSSPPWindows64.exe'
    if (-not (Test-Path -LiteralPath $portableExe) -or
        (Get-Item -LiteralPath $exe).LastWriteTimeUtc -ne (Get-Item -LiteralPath $portableExe).LastWriteTimeUtc) {
        Copy-Item -LiteralPath $exe -Destination $portableExe -Force
    }
    $assets=Join-Path $portable 'assets'
    if (-not (Test-Path -LiteralPath $assets)) {
        Copy-Item -LiteralPath (Join-Path (Split-Path $exe) 'assets') -Destination $assets -Recurse
    }
    $settings=Join-Path $portable 'memstick/PSP/SYSTEM'
    New-Item -ItemType Directory -Force $settings | Out-Null
    $original=Join-Path $memstick 'PSP/SYSTEM/ppsspp.ini'
    $copy=Join-Path $settings 'ppsspp.ini'
    if ((Test-Path -LiteralPath $original) -and -not (Test-Path -LiteralPath $copy)) { Copy-Item -LiteralPath $original -Destination $copy }
    if ($Psp1000) {
        # PPSSPP [SystemParam] PSPModel: 0 = PSP-1000 (32 MB), 1 = PSP-2000/3000 (64 MB).
        $ini=if (Test-Path -LiteralPath $copy) { [IO.File]::ReadAllText($copy) } else { '' }
        $ini=$ini -replace '(?m)^PSPModel\s*=.*\r?\n',''
        if ($ini -notmatch '(?m)^\[SystemParam\]\r?$') { $ini+="`n[SystemParam]`n" }
        $ini=$ini -replace '(?m)^\[SystemParam\]\r?$',"[SystemParam]`nPSPModel = 0"
        [IO.File]::WriteAllText($copy,$ini,[Text.UTF8Encoding]::new($false))
    }
    $exe=$portableExe;$memstick=Join-Path $portable 'memstick'
}
if ($Check) {
    "exe:      $exe"
    "version:  $((Get-Item $exe).VersionInfo.ProductVersion)"
    "memstick: $memstick (exists: $(Test-Path $memstick))"
    exit 0
}
if ($Play) {
    if ($Perf) { throw '-Play and -Perf are mutually exclusive' }
    if (-not $Eboot) { $Eboot=Join-Path $repo 'build/psp/package/PSP/GAME/HKPSP/EBOOT.PBP' }
    if (-not $Data) { $Data=Join-Path $repo 'build/psp/package/PSP/GAME/HKPSP/data' }
    if (Test-Path -LiteralPath (Join-Path $Data 'test-mode.cfg')) { throw 'Play data contains test-mode.cfg' }
}
# 2026-11-18 (enemy-c, cornifer-dialogue): a check run on data without test-mode.cfg plays interactively,
# never writes out/done.txt and only ends at the timeout. Stage with tools/psp/stage-data.ps1 -Test, or use -Play.
# Only the game itself needs it: fixture EBOOTs (tests/psp/test_hud_runtime.py) write done.txt on their own.
if (-not ($Play -or $Perf -or $Gpu -or $Diag -or $Check) -and $Data -and $Eboot -and
    -not (Test-Path -LiteralPath (Join-Path $Data 'test-mode.cfg'))) {
    $game = @((Join-Path $repo 'psp/EBOOT.PBP'), (Join-Path $repo 'build/psp/package/PSP/GAME/HKPSP/EBOOT.PBP')) |
        ForEach-Object { [IO.Path]::GetFullPath($_) }
    if ([IO.Path]::GetFullPath($Eboot) -in $game) {
        throw "No test-mode.cfg in ${Data}: stage it with tools/psp/stage-data.ps1 -Test (or pass -Play for interactive play)"
    }
}
if ($Perf -and -not $Eboot) { $Eboot=Join-Path $repo 'build/psp/EBOOT-PERF.PBP' }
if ($Gpu -and -not $Eboot) { $Eboot=Join-Path $repo 'build/psp/EBOOT-GPU.PBP' }
if ($Diag -and -not $Eboot) { $Eboot=Join-Path $repo 'build/psp/EBOOT-DIAG.PBP' }
if (-not $Eboot) { throw 'Pass -Eboot <path>, -Play or -Check.' }

# One run per PPSSPP memstick at a time (orchestrator 2026-11-15): all agents share this memstick, and
# concurrent runs deleted each other's HKPSP/data and out folders (flaky "file in use" / missing
# done.txt). Machine-wide mutex keyed by the memstick path; waits up to 30 min.
$lockKey = [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash([Text.Encoding]::UTF8.GetBytes(([IO.Path]::GetFullPath($memstick)).ToLower()))).Replace('-', '').Substring(0, 16)  # Windows PowerShell 5.1 has no HashData
$lock = [Threading.Mutex]::new($false, "Global\hkpsp-ppsspp-$lockKey")
try { if (-not $lock.WaitOne([TimeSpan]::FromMinutes(30))) { throw "PPSSPP memstick busy for 30 min: $memstick" } }
catch [Threading.AbandonedMutexException] { }  # a killed run left it; we own it now
try {
    # With the mutex held nobody may still run a game from this memstick: an emulator left by a
    # killed test (a -Play check orphaned at 08:50 locked HKPSP/EBOOT.PBP for 6 h on 2026-11-16) is
    # stopped. This also ends a manual -Play session on this memstick.
    $stick = [IO.Path]::GetFullPath($memstick).TrimEnd('\').ToLower()
    Get-CimInstance Win32_Process -Filter "Name like 'PPSSPP%'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -and $_.CommandLine.ToLower().Replace('/', '\').Contains("$stick\psp\game\") } |
        ForEach-Object {
            $leftover = Get-Process -Id $_.ProcessId -ErrorAction SilentlyContinue
            if ($leftover) {
                "stopping leftover PPSSPP pid $($leftover.Id) on this memstick"
                Stop-Process -InputObject $leftover -Force
                if (-not $leftover.WaitForExit(10000)) { throw 'Leftover PPSSPP did not exit; memstick remains in use' }
                $leftover.Dispose()
            }
        }
    # 2026-11-19 (recurring, 36 hits in one orchestrator session): a run killed during graphics start
    # leaves PSP/SYSTEM/FailedGraphicsBackends.txt; the next start then exits at once with no done.txt.
    # Only our own portable copies are cleaned, never the user's PPSSPP memstick.
    $failedBackends = Join-Path $memstick 'PSP/SYSTEM/FailedGraphicsBackends.txt'
    if ([IO.Path]::GetFullPath($memstick).StartsWith([IO.Path]::GetFullPath($repo), [StringComparison]::OrdinalIgnoreCase) -and
        (Test-Path -LiteralPath $failedBackends)) {
        "removing $failedBackends left by an interrupted run"
        Remove-Item -LiteralPath $failedBackends -Force
    }
    # 2026-11-19 (recurring): captures and boot checks were taken from an EBOOT older than the merged
    # sources. The game EBOOT must be newer than every C++ source and header it is built from.
    if ([IO.Path]::GetFullPath($Eboot) -eq [IO.Path]::GetFullPath((Join-Path $repo 'psp/EBOOT.PBP')) -and -not $AllowStaleEboot) {
        $built = (Get-Item -LiteralPath $Eboot).LastWriteTimeUtc
        $newer = @('psp/src', 'psp/include', 'src', 'include') | ForEach-Object { Join-Path $repo $_ } |
            Where-Object { Test-Path -LiteralPath $_ } |
            ForEach-Object { Get-ChildItem -LiteralPath $_ -Recurse -File -Include *.cpp, *.hpp, *.h, *.inc } |
            Where-Object { $_.LastWriteTimeUtc -gt $built } | Select-Object -First 3
        if ($newer) { throw "Stale EBOOT: $($newer.FullName -join ', ') newer than $Eboot. Rebuild (wsl bash tools/psp/wsl-make.sh psp) or pass -AllowStaleEboot." }
    }
    $gameName=if ($Diag) { 'HKPSPDIAG' } elseif ($Gpu) { 'HKPSPGPU' } elseif ($Perf) { 'HKPSPPERF' } else { 'HKPSP' }
    $game = Join-Path $memstick "PSP/GAME/$gameName"
    New-Item -ItemType Directory -Force $game | Out-Null
    Copy-Item $Eboot (Join-Path $game 'EBOOT.PBP') -Force
    if ($DiagConfig) {
        $diagTarget=Join-Path $game 'diag'
        if ([IO.Path]::GetFullPath($diagTarget) -ne [IO.Path]::GetFullPath("$memstick/PSP/GAME/HKPSPDIAG/diag")) { throw 'Unsafe diagnostic path' }
        if (Test-Path -LiteralPath $diagTarget) { Remove-Item -LiteralPath $diagTarget -Recurse -Force }
        New-Item -ItemType Directory -Force $diagTarget | Out-Null
        Copy-Item (Join-Path $DiagConfig '*') $diagTarget -Recurse -Force
    }
    if ($Data) {
        # Both EBOOTs use HKPSP/data; the profiler has only its own out folder.
        $targetData=[IO.Path]::GetFullPath((Join-Path $memstick 'PSP/GAME/HKPSP/data'))
        if ($targetData -ne [IO.Path]::GetFullPath("$memstick/PSP/GAME/HKPSP/data")) { throw 'Unsafe data path' }
        if (Test-Path -LiteralPath $targetData) { Remove-Item -LiteralPath $targetData -Recurse -Force }
        New-Item -ItemType Directory -Force $targetData | Out-Null
        Copy-Item (Join-Path $Data '*') $targetData -Recurse -Force
    }
    $capture = Join-Path $game 'out'
    if ([IO.Path]::GetFullPath($capture) -ne [IO.Path]::GetFullPath("$memstick/PSP/GAME/$gameName/out")) { throw 'Unsafe capture path' }
    if (Test-Path -LiteralPath $capture) { Remove-Item -LiteralPath $capture -Recurse -Force }

    $run = Join-Path $repo ("build/psp/run-" + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
    New-Item -ItemType Directory -Force $run | Out-Null
    $log = Join-Path $run 'ppsspp.log'
    if ($ExactPixels) {
        [IO.File]::WriteAllText((Join-Path $run 'renderer.txt'),"software`nnative_1x`n",[Text.Encoding]::ASCII)
        'Pixel parity: native 1x software rasterizer, isolated portable settings.'
    }
    # A boot whose memory map fails ("MemMap ... Failed at view") never starts the game and PPSSPP
    # stays open: it cost a 1,266 s timeout on 2026-11-16 with five agents running. Retry it once.
    for ($attempt = 1; $attempt -le 2; $attempt++) {
        if ($attempt -gt 1) {
            if (Test-Path -LiteralPath $capture) { Remove-Item -LiteralPath $capture -Recurse -Force }
            Move-Item -LiteralPath $log (Join-Path $run "ppsspp-attempt$($attempt-1).log") -Force
            "memory map failed at boot; retrying ($attempt/2)"
            Start-Sleep -Seconds 5
        }
        $proc = Start-Process $exe -ArgumentList @('--escape-exit', '--windowed', "--log=$log", (Join-Path $game 'EBOOT.PBP')) -WindowStyle Hidden -PassThru
        if ($Play) {
            "play pid: $($proc.Id)"
            "game: $game"
            "run: $run"
            if ($AliveSeconds -gt 0) {
                Start-Sleep -Seconds $AliveSeconds
                $proc.Refresh()
                "play alive: $(-not $proc.HasExited)"
                "play out: $(@(if (Test-Path -LiteralPath $capture) { Get-ChildItem -LiteralPath $capture | ForEach-Object Name }) -join ',')"
                if (Test-Path -LiteralPath $capture) { Copy-Item (Join-Path $capture '*') $run -Recurse -Force }
                if (-not $proc.HasExited) {
                    $proc.Kill()
                    if (-not $proc.WaitForExit(10000)) { throw 'PPSSPP did not exit; memstick remains in use' }
                }
            }
            return
        }
        # The game writes out/done.txt when its run is complete (PPSSPP returns to its menu on exit
        # instead of quitting), so stop as soon as the marker appears.
        $deadline = (Get-Date).AddSeconds($TimeoutSec)
        $bootFailed = $false; $polls = 0
        # error.txt (missing asset, assert) ends the run too: waiting for the timeout cost 18 min (2026-11-16).
        while (-not $proc.HasExited -and -not (Test-Path (Join-Path $capture 'done.txt')) -and
               -not (Test-Path (Join-Path $capture 'error.txt')) -and (Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds 250
            if ((++$polls % 8) -eq 0 -and -not (Test-Path $capture) -and (Test-Path $log) -and
                (Select-String -LiteralPath $log -Pattern 'MemMap\.cpp.*Failed at view' -Quiet)) { $bootFailed = $true; break }
        }
        if (-not $proc.HasExited) {
            Start-Sleep -Milliseconds 500  # let the game close its files
            $proc.Kill()
            if (-not $proc.WaitForExit(10000)) { throw 'PPSSPP did not exit; memstick remains in use' }
            if ($bootFailed) { 'memory map failed at boot (killed)' }
            elseif (Test-Path (Join-Path $capture 'done.txt')) { 'done marker seen' }
            elseif (Test-Path (Join-Path $capture 'error.txt')) { 'error.txt seen (killed)' }
            else { "timeout after $TimeoutSec s (killed)" }
        }
        if (-not $bootFailed) { break }
    }
    if (Test-Path $capture) { Copy-Item (Join-Path $capture '*') $run -Recurse -Force }
    if ($Diag -and (Test-Path (Join-Path $game 'diag'))) {
        Copy-Item (Join-Path $game 'diag') (Join-Path $run 'diag') -Recurse -Force
    }
    "run: $run"
    if (Test-Path $log) { Get-Content $log -Tail 20 }
} finally { $lock.ReleaseMutex(); $lock.Dispose() }
