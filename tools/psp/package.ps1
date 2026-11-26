# Build and assemble a play-mode memory-stick tree. Converted private assets are prerequisites.
# -Route: stage every route room (tools/psp/route-itinerary.md census; run
# regen-assets.ps1 -Only routerooms first). Default stays the Stage A legs 1-2 for agent iteration.
# -Eboot/-HostExe (player bundle, with -GameOnly): prebuilt game EBOOT and replay host, so no WSL or g++.
param([string]$Trace, [switch]$PerfSweep, [switch]$Route, [string[]]$Rooms, [switch]$GameOnly,
      [string]$Eboot, [string]$HostExe)
$ErrorActionPreference='Stop'
# pwsh -File passes a comma-separated room list as one string.
$Rooms = @($Rooms | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. (Join-Path $PSScriptRoot 'build-config.ps1')
$python = Get-HkpspPython
if ($GameOnly -and $PerfSweep) { throw '-GameOnly and -PerfSweep cannot be combined' }
# Eight variants through tools/psp/make.ps1: skipped when built from the same inputs (stamp), the rest
# in parallel (each variant has its own object directory and outputs; make.ps1 locks per output).
# -PerfSweep (opt-in): the HKPSPPERF EBOOT runs all 11 CPU configurations instead of the shipped one.
$make=Join-Path $PSScriptRoot 'make.ps1'
$perfFlags=if ($PerfSweep) { @('PERF=1','PERF_SWEEP=1') } else { @('PERF=1') }
$perfEboot=if ($PerfSweep) { 'build/psp/EBOOT-PERF-SWEEP.PBP' } else { 'build/psp/EBOOT-PERF.PBP' }
$variants=@(@(),$perfFlags,@('GPU_ATTRIB=1'),@('DIAG=1'),@('BUILD_PRX=1'),@('BUILD_PRX=1','PERF=1'),@('BUILD_PRX=1','GPU_ATTRIB=1'),@('BUILD_PRX=1','DIAG=1'))
if ($Eboot -and -not $GameOnly) { throw '-Eboot is for the -GameOnly player package' }
$gameEboot = if ($Eboot) { (Get-Item -LiteralPath $Eboot -ErrorAction Stop).FullName } else { Join-Path $repo 'psp/EBOOT.PBP' }
$variantCount = if ($Eboot) { 0 } elseif ($GameOnly) { 1 } else { $variants.Count }
# Works in Windows PowerShell 5.1 too (tests call this script through `powershell`): no -Parallel.
$pwsh=(Get-Command pwsh -ErrorAction Stop).Source
$logDir=Join-Path $repo 'build/psp/logs'; New-Item -ItemType Directory -Force $logDir | Out-Null
$jobs=@()
for ($i=0; $i -lt $variantCount; $i++) {
    while (@($jobs | Where-Object { -not $_.P.HasExited }).Count -ge 3) { Start-Sleep -Milliseconds 200 }
    $flags=@($variants[$i]); $out=Join-Path $logDir "package-make-$i.log"
    $argList=@('-NoProfile','-File',('"'+$make+'"')) + $flags
    $p=Start-Process $pwsh -ArgumentList $argList -NoNewWindow -PassThru -RedirectStandardOutput $out -RedirectStandardError "$out.err"
    $null=$p.Handle  # keep the handle, or ExitCode stays empty after exit
    $jobs+=[pscustomobject]@{ P=$p; Flags=($flags -join ' '); Log=$out }
}
foreach ($j in $jobs) {
    $j.P.WaitForExit()
    $text=((Get-Content -LiteralPath $j.Log -ErrorAction SilentlyContinue) + (Get-Content -LiteralPath "$($j.Log).err" -ErrorAction SilentlyContinue)) -join "`n"
    if ($j.P.ExitCode -ne 0) { $text; throw "PSP build failed ($($j.Flags)): $($j.P.ExitCode)" }
    ($text.Trim() -split "`n")[-1]
}
if (-not $GameOnly) {
    $psplink=Join-Path $repo 'build/psp/psplink/PSP/GAME/PSPLINK'
    if (-not (Test-Path -LiteralPath (Join-Path $psplink 'EBOOT.PBP'))) {
        $builder=Join-Path $PSScriptRoot 'psplink-build.sh'
        $wslBuilder='/mnt/'+$builder.Substring(0,1).ToLower()+($builder.Substring(2) -replace '\\','/')
        & wsl -d Ubuntu-24.04 -- bash $wslBuilder
        if ($LASTEXITCODE -ne 0) { throw "PSPLINK build failed: $LASTEXITCODE" }
    }
    $provenance=Get-Content -Raw (Join-Path $repo 'build/psp/psplink/provenance.json') | ConvertFrom-Json
    $runtime=@('EBOOT.PBP','psplink.prx','psplink_user.prx','usbhostfs.prx','usbgdb.prx','psplink.ini')
    if (@(Get-ChildItem -LiteralPath $psplink -Force).Count -ne $runtime.Count) { throw 'Unexpected PSPLINK staging contents' }
    foreach ($name in $runtime) {
        # Use .NET directly: nested Windows PowerShell may inherit a PowerShell 7 module path.
        $sha=[Security.Cryptography.SHA256]::Create()
        try { $digest=[BitConverter]::ToString($sha.ComputeHash([IO.File]::ReadAllBytes((Join-Path $psplink $name)))) -replace '-','' }
        finally { $sha.Dispose() }
        if ($digest -ne $provenance.files.$name) {
            throw "PSPLINK staging hash mismatch: $name"
        }
    }
}
if (-not $Trace) {
    $hostArgs = @{}
    if ($HostExe) { $hostArgs['HostExe'] = $HostExe }
    & (Join-Path $PSScriptRoot 'host-build.ps1') -Run -Frames 1 -Every 1 -Fast @hostArgs
    $Trace=Join-Path $repo $(if ($IsWindows -or $PSVersionTable.PSEdition -eq 'Desktop') { 'build/psp/host/assets.tsv' } else { 'build/psp/host-linux/assets.tsv' })
}
$routeTrace=Join-Path $repo 'build/psp/rooms/package-assets.tsv'
# A startup trace cannot observe future rooms or shared scene-loader sidecars.
$stageArgs=@('--trace', $Trace, '--out', $routeTrace)
if ($Route) { $stageArgs += @('--stage', (Join-Path $repo 'tools/psp/route-itinerary.md')) }
if ($Rooms) {
    & $python -I (Join-Path $PSScriptRoot 'room_packages.py') --rooms @Rooms
    if ($LASTEXITCODE -ne 0) { throw "Additional room conversion failed: $LASTEXITCODE" }
    $stageArgs += @('--rooms') + $Rooms
}
& $python -I (Join-Path $PSScriptRoot 'room_stage.py') @stageArgs
if ($LASTEXITCODE -ne 0) { throw "Route dependency staging failed: $LASTEXITCODE" }
if ($GameOnly) {
    & (Join-Path $PSScriptRoot 'stage-data.ps1') -Trace $routeTrace -GameOnly
    $game = Join-Path $repo 'build/psp/package-game/PSP/GAME/HKPSP'
    Copy-Item -LiteralPath $gameEboot -Destination (Join-Path $game 'EBOOT.PBP') -Force
    "Game package: $game"
    return
}
& (Join-Path $PSScriptRoot 'stage-data.ps1') -Trace $routeTrace
$package=[IO.Path]::GetFullPath((Join-Path $repo 'build/psp/package'))
if ($package -ne [IO.Path]::GetFullPath("$repo/build/psp/package")) { throw 'Unsafe package path' }
if (Test-Path -LiteralPath $package) { Remove-Item -LiteralPath $package -Recurse -Force }
$game=Join-Path $package 'PSP/GAME/HKPSP'
New-Item -ItemType Directory -Force $game | Out-Null
Copy-Item -LiteralPath (Join-Path $repo 'psp/EBOOT.PBP') -Destination (Join-Path $game 'EBOOT.PBP')
Copy-Item -LiteralPath (Join-Path $repo 'build/psp/data') -Destination (Join-Path $game 'data') -Recurse
$perf=Join-Path $package 'PSP/GAME/HKPSPPERF'
New-Item -ItemType Directory -Force $perf | Out-Null
Copy-Item -LiteralPath (Join-Path $repo $perfEboot) -Destination (Join-Path $perf 'EBOOT.PBP')
$gpu=Join-Path $package 'PSP/GAME/HKPSPGPU'
New-Item -ItemType Directory -Force $gpu | Out-Null
Copy-Item -LiteralPath (Join-Path $repo 'build/psp/EBOOT-GPU.PBP') -Destination (Join-Path $gpu 'EBOOT.PBP')
$diag=Join-Path $package 'PSP/GAME/HKPSPDIAG'
New-Item -ItemType Directory -Force (Join-Path $diag 'diag') | Out-Null
Copy-Item -LiteralPath (Join-Path $repo 'build/psp/EBOOT-DIAG.PBP') -Destination (Join-Path $diag 'EBOOT.PBP')
# Live play starts at the measured climb; tapes remain opt-in under presets/.
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'diag-presets/tutorial-climb/start.cfg') -Destination (Join-Path $diag 'diag/start.cfg')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'diag-presets') -Destination (Join-Path $diag 'presets') -Recurse
Copy-Item -LiteralPath $psplink -Destination (Join-Path $package 'PSP/GAME/PSPLINK') -Recurse
$prx=Join-Path $package 'prx'
New-Item -ItemType Directory -Force $prx | Out-Null
Copy-Item -LiteralPath (Join-Path $repo 'psp/hkpsp.prx') -Destination $prx
Copy-Item -LiteralPath (Join-Path $repo 'build/psp/hkpsp-perf.prx') -Destination $prx
Copy-Item -LiteralPath (Join-Path $repo 'build/psp/hkpsp-gpu.prx') -Destination $prx
Copy-Item -LiteralPath (Join-Path $repo 'build/psp/hkpsp-diag.prx') -Destination $prx
$files=@(Get-ChildItem -LiteralPath $package -Recurse -File)
$bytes=($files | Measure-Object Length -Sum).Sum
"Package: $package"
"Package size: $bytes bytes; files=$($files.Count)"
'Copy the PSP folder into the root of the PSP memory stick; launch Game > Memory Stick > HKPSP Tutorial_01.'
'Play: tools/psp/ppsspp.ps1 -Play'
if ($PerfSweep) { 'Profile: launch HKPSP perf SWEEP (11 x 600 tape frames, shared HKPSP/data); collect with tools/psp/read-perf.ps1 -Drive I:' }
else { 'Profile: launch HKPSP perf (shipped config, 600 tape frames, shared HKPSP/data); collect with tools/psp/read-perf.ps1 -Drive I:' }
'GPU attribution: launch HKPSPGPU (24 x 150 tape frames, shared HKPSP/data); collect with tools/psp/read-perf.ps1 -Gpu -Drive I:'
'Diagnostics: launch HKPSPDIAG, press Select+Start; collect with tools/psp/diag-collect.ps1 -Source I:/'
