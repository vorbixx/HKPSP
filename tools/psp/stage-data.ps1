# Stage only logical files observed in a successful host run's Platform::path trace.
param([string]$Trace, [switch]$Test, [switch]$GameOnly,
    [ValidateRange(1,1000000)][int]$Frames=300,
    [ValidateRange(1,1000000)][int]$CaptureInterval=30)
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. (Join-Path $PSScriptRoot 'build-config.ps1')
# The build's Python (HKPSP_PYTHON): a player bundle has no python on PATH.
$python=Get-HkpspPython
# 2026-11-19 (recurring): checks ran on converted data older than the merged converters.
$staleGroups = & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'regen-assets.ps1') -ListStale 2>&1 | Select-Object -Last 1
if ("$staleGroups" -like 'stale:*') {
    Write-Warning "converted data is older than its converters ($staleGroups); run tools/psp/regen-assets.ps1 -Only stale"
}
if (-not $Trace) {
    $Trace=Join-Path $repo 'build/psp/host/assets.tsv'
    # 2026-11-19: after a regen the old trace missed ability-page-*.hpt and the boot failed with
    # "Missing asset". The trace must be newer than the last regen of the converted data.
    $stamps=Join-Path $repo '_private/converted-psp/.regen-stamps.json'
    if ((Test-Path -LiteralPath $Trace) -and (Test-Path -LiteralPath $stamps) -and
        (Get-Item -LiteralPath $Trace).LastWriteTimeUtc -lt (Get-Item -LiteralPath $stamps).LastWriteTimeUtc) {
        Write-Warning 'host trace is older than the converted data; refresh it: tools/psp/host-build.ps1 -Run -Fast (or package.ps1)'
    }
}
$files=@{}
$rows=@(Get-Content -LiteralPath $Trace)
foreach ($line in $rows) {
    $fields=$line -split "`t",2
    if ($fields.Count -ne 2 -or $fields[0] -cnotmatch '^(((rooms|scenes)/[a-zA-Z0-9_-]+/)?[a-zA-Z0-9_-]+\.[a-zA-Z0-9]+|rooms/[a-zA-Z0-9_-]+/static-room-visual\.bin\.switches|route-tolls/[a-zA-Z0-9_-]+\.bin)$') {
        throw "Invalid host asset trace row: $line"
    }
    $name=$fields[0]; $source=[IO.Path]::GetFullPath($fields[1])
    # Sidecars must come from the same converted package as the PSR1, never
    # from the original 916-sprite HK3DS metadata (even for an older trace).
    $leaf=($name -split '/')[-1]
    $prefix=$name.Substring(0,$name.Length-$leaf.Length)
    if ($leaf -in @('static-plants.bin','static-room-dynamics.bin','static-room-ambient.bin','static-grass.bin')) {
        $roomRows=@($rows | Where-Object { $_.StartsWith("${prefix}static-room-visual.bin`t",[StringComparison]::Ordinal) } | Select-Object -Unique)
        if ($roomRows.Count -ne 1) { throw "Expected one traced PSP room table for converted sidecars: $prefix" }
        $roomSource=[IO.Path]::GetFullPath(($roomRows[0] -split "`t",2)[1])
        $generated=Join-Path (Split-Path -Parent $roomSource) $leaf
        if ($source -cne $generated) { throw "Sidecar does not match traced PSP package: $name" }
    }
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing traced file: $source" }
    if ($files.ContainsKey($name)) {
        if ($files[$name].name -cne $name -or $files[$name].source -cne $source) {
            throw "Conflicting logical name: $name"
        }
    } else { $files[$name]=@{name=$name;source=$source} }
    # HPT2 palettes are an implicit reader dependency, absent from Platform::path.
    if ($name -like '*.hpt') {
        $stream=[IO.File]::OpenRead($source)
        try { $magic=New-Object byte[] 4; $null=$stream.Read($magic,0,4) } finally { $stream.Dispose() }
        if ([Text.Encoding]::ASCII.GetString($magic) -eq 'HPT2') {
            $palette='palettes.pcl'
            if ($name -match '^(.+)-page-\d+\.hpt$') { $palette=$Matches[1]+'-palettes.pcl' }
            $paletteSource=Join-Path (Split-Path -Parent $source) 'palettes.pcl'
            if (-not (Test-Path -LiteralPath $paletteSource -PathType Leaf)) { throw "Missing HPT2 palettes: $source" }
            if ($files.ContainsKey($palette) -and $files[$palette].source -cne $paletteSource) { throw "Conflicting CLUT namespace: $palette" }
            $files[$palette]=@{name=$palette;source=$paletteSource}
        }
    }
    if ($name -ceq 'tutorial-room.bin') {
        # TerrainBank::load opens these beside the terrain bank itself, not through the
        # platform, so the host trace never names them (2026-11-25: the Steam King's Pass
        # has a breakable wall and failed to boot without terrain-walls.bin).
        foreach ($sidecar in 'terrain-walls.bin','terrain-motion.bin','terrain-levers.bin') {
            $sidecarSource=Join-Path (Split-Path -Parent $source) $sidecar
            if (Test-Path -LiteralPath $sidecarSource -PathType Leaf) {
                $sidecarName=$sidecar
                if ($files.ContainsKey($sidecarName) -and $files[$sidecarName].source -cne $sidecarSource) { throw "Conflicting terrain sidecar: $sidecarName" }
                $files[$sidecarName]=@{name=$sidecarName;source=$sidecarSource}
            }
        }
    }
    if ($name -like 'audio-*.pcm') {
        $pack=Join-Path (Split-Path -Parent $source) 'audio.pak'
        if (Test-Path -LiteralPath $pack -PathType Leaf) {
            if ($files.ContainsKey('audio.pak') -and $files['audio.pak'].source -cne $pack) { throw 'Conflicting audio packs' }
            $files['audio.pak']=@{name='audio.pak';source=$pack}
        }
    }
}
if (-not $files.Count) { throw 'Empty host asset trace' }
$output = if ($GameOnly) { 'build/psp/package-game/PSP/GAME/HKPSP/data' } else { 'build/psp/data' }
$destination=[IO.Path]::GetFullPath((Join-Path $repo $output))
# This script replaces only its own fixed staging directory, after validating the trace.
for ($parent = $destination; $parent -ne $repo; $parent = Split-Path -Parent $parent) {
    $item = Get-Item -LiteralPath $parent -ErrorAction SilentlyContinue
    if ($item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Unsafe staging path: $parent" }
}
if (Test-Path -LiteralPath $destination) {
    $resolved = (Resolve-Path -LiteralPath $destination -ErrorAction Stop).Path
    if ($resolved -ne $destination) { throw "Unsafe staging path: $resolved" }
}
if (Test-Path -LiteralPath $destination) { Remove-Item -LiteralPath $destination -Recurse -Force }
New-Item -ItemType Directory -Force $destination | Out-Null
foreach ($name in $files.Keys) {
    $target=Join-Path $destination $name
    New-Item -ItemType Directory -Force (Split-Path -Parent $target) | Out-Null
    Copy-Item -LiteralPath $files[$name].source -Destination $target
}
if (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'page_demand.py')) {
    & $python -I (Join-Path $PSScriptRoot 'page_demand.py') --manifests $destination
    if ($LASTEXITCODE) { throw 'page_demand.py failed' }
}
# textures.pak packs the top-level data/*.hpt only (measured 13.5 ms saving per page-file open);
# the PSP reader uses it only for pages of that directory, so room subfolders (which reuse page
# names) always load their own loose files (2026-11-17 route test: a basename lookup read the
# Tutorial's static-room-page-9 for Town's).
if (Get-ChildItem -LiteralPath $destination -Filter '*.hpt' -File) {
    & $python -I (Join-Path $PSScriptRoot 'texture_pack.py') $destination
    if ($LASTEXITCODE) { throw 'texture_pack.py failed' }
}
if ($Test) {
    [IO.File]::WriteAllText((Join-Path $destination 'test-mode.cfg'),
        "frames=$Frames`ncapture_interval=$CaptureInterval`n",[Text.Encoding]::ASCII)
}
$bytes=(Get-ChildItem -LiteralPath $destination -File -Recurse | Measure-Object Length -Sum).Sum
"Staged $($files.Count) traced files; bytes=$bytes; data=$destination"
