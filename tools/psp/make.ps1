# PSP make with an input stamp (orchestrator 2026-11-15, user: "tests are too long"). A no-change
# `make` over /mnt/c costs 8-13 s per variant (6 variants per package = ~50 s, in 10 test modules).
# This wrapper hashes the build inputs with Windows git (tracked index + working-tree diff + untracked
# files under psp/, src/, include/) plus the make flags, and skips WSL make when the variant's output
# exists and was built from the same stamp. Any build writes the stamp for its output, so a build
# with other flags (e.g. PSP_LARGE_MEMORY=0) into the same output invalidates the cached one.
#   pwsh -NoProfile -File tools/psp/make.ps1 [PERF=1 [PERF_SWEEP=1]] [GPU_ATTRIB=1] [BUILD_PRX=1] [other make args]
#   -Force always runs make. Log of a real build: build/psp/logs/make-<variant>.log
param([switch]$Force, [Parameter(ValueFromRemainingArguments)][string[]]$Flags)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$Flags = @($Flags | Where-Object { $_ })
$git = 'C:/Program Files/Git/cmd/git.exe'
$perf = $Flags -contains 'PERF=1'; $gpu = $Flags -contains 'GPU_ATTRIB=1'; $prx = $Flags -contains 'BUILD_PRX=1'
$sweep = $Flags -contains 'PERF_SWEEP=1'
$diag = $Flags -contains 'DIAG=1'
$variant = if ($diag) { 'diag' } elseif ($gpu) { 'gpu' } elseif ($sweep) { 'perf-sweep' } elseif ($perf) { 'perf' } else { 'game' }
if ($prx) { $variant += '-prx' }
$output = @{ 'game' = 'psp/EBOOT.PBP'; 'perf' = 'build/psp/EBOOT-PERF.PBP'; 'perf-sweep' = 'build/psp/EBOOT-PERF-SWEEP.PBP'; 'gpu' = 'build/psp/EBOOT-GPU.PBP'
    'diag' = 'build/psp/EBOOT-DIAG.PBP'; 'diag-prx' = 'build/psp/hkpsp-diag.prx'
    'game-prx' = 'psp/hkpsp.prx'; 'perf-prx' = 'build/psp/hkpsp-perf.prx'; 'gpu-prx' = 'build/psp/hkpsp-gpu.prx' }[$variant]
$stampDir = Join-Path $repo 'build/psp/stamps'; New-Item -ItemType Directory -Force $stampDir | Out-Null
$stampFile = Join-Path $stampDir "$variant.txt"
$logDir = Join-Path $repo 'build/psp/logs'; New-Item -ItemType Directory -Force $logDir | Out-Null

function Get-InputStamp {
    $paths = @('psp', 'src', 'include')
    $text = [Text.StringBuilder]::new()
    [void]$text.AppendLine((& $git -C $repo ls-files -s -- @paths | Out-String))
    [void]$text.AppendLine((& $git -C $repo diff HEAD --no-color -- @paths | Out-String))
    foreach ($f in @(& $git -C $repo ls-files -o --exclude-standard -- @paths)) {
        $full = Join-Path $repo $f
        if (Test-Path -LiteralPath $full -PathType Leaf) { [void]$text.AppendLine("$f $((Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash)") }
    }
    [void]$text.AppendLine(($Flags | Sort-Object) -join ' ')
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($text.ToString()))) -replace '-', '' }
    finally { $sha.Dispose() }
}

# One build per output at a time in this tree (parallel tests share build/psp).
$key = [BitConverter]::ToString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes(($repo + '|' + $variant).ToLower()))).Replace('-', '').Substring(0, 16)
$mutex = [Threading.Mutex]::new($false, "hkpsp-make-$key")
[void]$mutex.WaitOne()
try {
    $stamp = Get-InputStamp
    $outPath = Join-Path $repo $output
    if (-not $Force -and (Test-Path -LiteralPath $outPath) -and (Test-Path -LiteralPath $stampFile) -and
        ((Get-Content -LiteralPath $stampFile -Raw).Trim() -eq $stamp)) {
        "make ${variant}: up to date (stamp)"; exit 0
    }
    Remove-Item -LiteralPath $stampFile -ErrorAction SilentlyContinue
    $wrapper = Join-Path $repo 'tools/psp/wsl-make.sh'
    $wslWrapper = '/mnt/' + $wrapper.Substring(0, 1).ToLower() + ($wrapper.Substring(2) -replace '\\', '/')
    $log = Join-Path $logDir "make-$variant.log"
    & wsl -d Ubuntu-24.04 -- bash $wslWrapper psp @Flags *> $log
    $code = $LASTEXITCODE
    if ($code -ne 0) { Get-Content -LiteralPath $log -Tail 30; throw "PSP make $variant failed: $code (log $log)" }
    if (-not (Test-Path -LiteralPath $outPath)) { throw "PSP make ${variant}: missing output $output" }
    # Inputs changed during the build -> do not record a stamp that does not match the output.
    if ((Get-InputStamp) -eq $stamp) { Set-Content -LiteralPath $stampFile -Value $stamp -NoNewline }
    "make ${variant}: built"
} finally { $mutex.ReleaseMutex(); $mutex.Dispose() }
