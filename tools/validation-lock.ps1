# Admission for heavy agent commands on this PC, so agents need no lock code of their own.
# Every worktree converts into its OWN _private/converted; only the junctioned _private entries
# (research, tools, original, ...) are shared. So:
#   tools/validation-lock.ps1 -Write -- python tools/regen.py
#       regen/converters writing this worktree's converted: no lock. Waits only while free RAM
#       is below HK3DS_WRITE_MIN_FREE_GB (default 4), then runs beside any number of others.
#   tools/validation-lock.ps1 -Exclusive -- <AssetRipper extraction into _private/research>
#       writes into a shared _private entry or the one AssetRipper port: one at a time, through
#       C:/Projects/HK3DS/_private/shared-private.lock.
#   tools/validation-lock.ps1 -Read -- <command>
#       runs at once (kept for older prompts). Checks, host tests and test-emulator.ps1 read only
#       their own worktree and need no lock at all.
# None of the modes opens the retired _private/pc2-validation.lock: 2026-10-26 one agent's
# 40-min regen held it exclusively, and hand-written helpers still lock it, so every other
# agent queued behind a single command.
# Waits without a timeout by default (-TimeoutMinutes 0); exits with the command's exit code.
# No param() block: pwsh -File would bind the wrapped command's own -Flags to this script.
$ErrorActionPreference = 'Stop'
$mode = ''; $TimeoutMinutes = 0
# A worktree's .git file points at <main>/.git/worktrees/<slug>.
# Resolve shared configuration even when the caller overrides the lock path.
$checkout = Split-Path -Parent $PSScriptRoot
$main = $checkout
$dotGit = Join-Path $checkout '.git'
if (Test-Path -LiteralPath $dotGit -PathType Leaf) {
    $gitDir = ((Get-Content -LiteralPath $dotGit -TotalCount 1) -replace '^gitdir:\s*', '').Trim()
    $main = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $gitDir))
}
# A machine-local override (user 2026-11-01: the 15 GB laptop held every check at the 4 GB gate) lives in the
# main checkout's _private, outside Git, and applies to running agents without restarting them.
. (Join-Path $PSScriptRoot 'heavy-admission.ps1')
# "commit:<GB>" gates on free commit (RAM + page file) instead of physical RAM: work may page, but never
# runs the machine out of commit (user 2026-11-01: "the laptop has a page file").
$gateText = Get-MachineMemoryGate
$minCommitGB = if (-not $env:HK3DS_WRITE_MIN_FREE_GB -and $gateText -match '^commit:([\d.]+)$') { [double]$Matches[1] } else { 0 }
$minFreeGB = if ($env:HK3DS_WRITE_MIN_FREE_GB) { [double]$env:HK3DS_WRITE_MIN_FREE_GB }
             elseif ($minCommitGB) { 0 }
             elseif ($gateText) { [double]$gateText }
             else { 4 }
$rest = @($args)
while ($rest.Count) {
    $a = [string]$rest[0]
    if ($a -in '-Write', '-Read', '-Exclusive') {
        if ($mode) { throw 'Pass exactly one of -Write, -Read or -Exclusive.' }
        $mode = $a.TrimStart('-')
    }
    elseif ($a -eq '-TimeoutMinutes') { $TimeoutMinutes = [double]$rest[1]; $rest = @($rest | Select-Object -Skip 1) }
    elseif ($a -eq '--') { $rest = @($rest | Select-Object -Skip 1); break }
    else { break }
    $rest = @($rest | Select-Object -Skip 1)
}
$Command = $rest
if (-not $mode) { throw 'Pass exactly one of -Write, -Read or -Exclusive.' }
if (-not $Command) { throw 'Pass the command after --.' }

$lock = $env:HK3DS_SHARED_PRIVATE_LOCK
if (-not $lock) {
    $lock = Join-Path $main '_private/shared-private.lock'
}

$deadline = if ($TimeoutMinutes -gt 0) { (Get-Date).AddMinutes($TimeoutMinutes) } else { [datetime]::MaxValue }
$announced = $false
function Wait-Busy($what) {
    if ((Get-Date) -gt $deadline) { throw "still waiting after $TimeoutMinutes min for $what" }
    if (-not $script:announced) { Write-Output "waiting for $what"; $script:announced = $true }
    Start-Sleep -Seconds 10
}

$handle = $null
if ($mode -eq 'Exclusive') {
    while (-not $handle) {
        try { $handle = [System.IO.File]::Open($lock, [System.IO.FileMode]::OpenOrCreate, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None) }
        catch [System.IO.IOException] { Wait-Busy "$lock (exclusive)" }
    }
} elseif ($mode -eq 'Write') {
    # A worktree whose converted is a link writes another checkout's assets (2026-10-30: three worktrees
    # linked to main's converted). tools/worktree-private.ps1 replaces such a link with an own copy.
    $here = (Get-Location).Path
    $convertedLink = Get-Item -LiteralPath (Join-Path $here '_private/converted') -Force -ErrorAction SilentlyContinue
    if ($convertedLink -and $convertedLink.LinkType -and (Test-Path -LiteralPath (Join-Path $here '.git') -PathType Leaf)) {
        throw "Refusing -Write: $here/_private/converted is a link to $($convertedLink.Target); run tools/worktree-private.ps1 -Checkout $here first."
    }
    # RAM (strict) and CPU (soft) admission plus BelowNormal priority: tools/heavy-admission.ps1.
    Wait-HeavyAdmission -MinFreeGB $minFreeGB -MinCommitGB $minCommitGB -Deadline $deadline
    Set-HeavyPriority
    # The commit gate admitted this command; a child regen.py must not reapply its own 4 GB physical-RAM
    # floor (2026-11-01: laptop regens waited there after admission). regen.py reads this variable.
    if ($minCommitGB -and -not $env:HK3DS_WRITE_MIN_FREE_GB) { $env:HK3DS_WRITE_MIN_FREE_GB = '0' }
    $env:HK3DS_HEAVY_ADMITTED = '1'  # a check.ps1 run inside this command is not admitted twice
}
. (Join-Path $PSScriptRoot 'error-mode.ps1')
Disable-ErrorDialogs  # a crashed child must exit, not hold the lock under a modal error box
$code = 1
try {
    $exe = [string]$Command[0]
    # Keep arguments typed as an array: PowerShell can unwrap one argument and splat a string as characters.
    [string[]]$arguments = @($Command | Select-Object -Skip 1)
    $global:LASTEXITCODE = $null
    & $exe @arguments
    $code = if ($null -ne $LASTEXITCODE) { $LASTEXITCODE } elseif ($?) { 0 } else { 1 }
} finally {
    if ($handle) { $handle.Dispose() }
}
exit $code
