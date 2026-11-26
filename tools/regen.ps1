#Requires -Version 7.0
[CmdletBinding()]
param([switch]$DryRun, [switch]$List, [switch]$NoCache, [switch]$PspRoute, [string]$Python = $env:HK3DS_PYTHON,
      [ValidateNotNullOrEmpty()][string]$Only)
$ErrorActionPreference = 'Stop'
if (-not $Python) {
    $bundledPython = Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
    $Python = if (Test-Path -LiteralPath $bundledPython) { $bundledPython } else { 'python' }
}
$arguments = @('-E', "$PSScriptRoot/regen.py")
if ($DryRun) { $arguments += '--dry-run' }
if ($List) { $arguments += '--list' }
if ($NoCache) { $arguments += '--no-cache' }
if ($PspRoute) { $arguments += '--psp-route' }
if ($PSBoundParameters.ContainsKey('Only')) { $arguments += @('--only', $Only) }
& $Python @arguments
if ($LASTEXITCODE -ne 0) { throw "Asset regeneration failed ($LASTEXITCODE). See build/regen/logs/." }
