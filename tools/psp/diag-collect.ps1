# USB root, HKPSPDIAG/diag, numbered capture, or copied PSPLINK capture directory.
param([Parameter(Mandatory=$true)][string]$Source, [string]$Output)
$ErrorActionPreference='Stop'
$root=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $Output) { $Output=Join-Path $root ('build/psp/diag-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff')) }
python -I (Join-Path $PSScriptRoot 'diag_collect.py') $Source --output $Output
if ($LASTEXITCODE -ne 0) { throw "Diagnostic collection failed ($LASTEXITCODE)" }
