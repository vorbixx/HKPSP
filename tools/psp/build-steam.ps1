#Requires -Version 7.0
[CmdletBinding()]
param([string]$SteamGame, [switch]$Check, [switch]$Route, [switch]$AllRooms, [ValidateRange(1, 16)][int]$Jobs = 4,
      [switch]$UseInstalledFiles,
      [ValidateRange(1, 65535)][int]$AssetRipperPort, [string]$AssetRipperExe,
      [string]$IlspyExe, [string]$FreeTypeDll = $env:HKPSP_FREETYPE_DLL,
      [string]$Cecil, [string]$DotnetRoot = $(if ($IsWindows) { Join-Path $env:ProgramFiles 'dotnet' } else { '/usr/share/dotnet' }),
      [string]$Prebuilt)

$ErrorActionPreference = 'Stop'
# -AllRooms builds every PSP room (tools/psp/all-rooms.txt): the route exports
# and extra sources plus the remaining rooms of the map.
if ($AllRooms) { $Route = $true }
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. (Join-Path $PSScriptRoot 'build-config.ps1')
if ($AssetRipperPort -and $AssetRipperExe) { throw 'Specify one AssetRipper option.' }
if ($AssetRipperExe) {
    $AssetRipperExe = (Get-Item -LiteralPath $AssetRipperExe -ErrorAction Stop).FullName
    if (-not (Test-Path -LiteralPath $AssetRipperExe -PathType Leaf)) { throw 'Select the AssetRipper executable.' }
}
if ($IlspyExe) {
    $IlspyExe = (Get-Item -LiteralPath $IlspyExe -ErrorAction Stop).FullName
    if (-not (Test-Path -LiteralPath $IlspyExe -PathType Leaf)) { throw 'Select the ilspycmd executable.' }
} else {
    $ilspyCommand = Get-Command ilspycmd -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($ilspyCommand) { $IlspyExe = $ilspyCommand.Source }
}
if ($FreeTypeDll) {
    $FreeTypeDll = (Get-Item -LiteralPath $FreeTypeDll -ErrorAction Stop).FullName
    if (-not (Test-Path -LiteralPath $FreeTypeDll -PathType Leaf)) { throw 'Select the FreeType DLL.' }
}
$archive = Join-Path $repo '_private/original/Hollow Knight'
if ($UseInstalledFiles -and -not $SteamGame) { throw '-UseInstalledFiles requires -SteamGame.' }
$steam = if ($SteamGame) { (Resolve-Path -LiteralPath $SteamGame -ErrorAction Stop).Path } else { $archive }
$steamManifest = Join-Path (Split-Path -Parent $archive) 'appmanifest_367520.acf'
if ($SteamGame) {
    # Steam keeps the manifest in steamapps, two folders above the game.
    $steamParent = Split-Path -Parent $steam
    foreach ($folder in @((Split-Path -Parent $steamParent), $steamParent)) {
        if ($folder) {
            $candidate = Join-Path $folder 'appmanifest_367520.acf'
            if (Test-Path -LiteralPath $candidate -PathType Leaf) { $steamManifest = $candidate; break }
        }
    }
}
# The boss exporters record the Steam build ID from this manifest.
if (-not (Test-Path -LiteralPath $steamManifest -PathType Leaf)) {
    throw 'The Steam appmanifest_367520.acf was not found. Pass the installed game folder with -SteamGame.'
}
if ($Cecil) {
    $Cecil = (Resolve-Path -LiteralPath $Cecil -ErrorAction Stop).Path
    $rocks = Join-Path (Split-Path -Parent $Cecil) 'Mono.Cecil.Rocks.dll'
    if (-not (Test-Path -LiteralPath $Cecil -PathType Leaf) -or
        -not (Test-Path -LiteralPath $rocks -PathType Leaf) -or
        -not (Test-Path -LiteralPath (Join-Path $DotnetRoot 'dotnet.exe') -PathType Leaf)) {
        throw 'Title capture requires Mono.Cecil.dll, Mono.Cecil.Rocks.dll and a .NET SDK.'
    }
}
$gameData = Join-Path $steam 'hollow_knight_Data'
$required = @('globalgamemanagers.assets', 'resources.assets', 'resources.assets.resS',
              'Managed/Assembly-CSharp.dll', 'level1', 'level6', 'sharedassets0.assets',
              'sharedassets1.assets', 'sharedassets1.assets.resS')
$missing = @($required | Where-Object { -not (Test-Path -LiteralPath (Join-Path $gameData $_) -PathType Leaf) })
if ($missing.Count) {
    throw "Missing Windows game files in $gameData ($($missing -join ', ')). Pass -SteamGame the folder opened by Steam's Installed Files > Browse."
}

$python = Get-HkpspPython
& $python -I -c 'import numpy, PIL, UnityPy, soundfile'
if ($LASTEXITCODE) { throw 'Install NumPy, Pillow, UnityPy and soundfile into the Python selected by HKPSP_PYTHON.' }
if ($Route -and $AssetRipperExe -and -not (Test-Path -LiteralPath (Join-Path $repo '_private/false-knight-source-art/fullres/manifest.json') -PathType Leaf)) {
    & $python -I -c 'import TypeTreeGeneratorAPI'
    if ($LASTEXITCODE) { throw 'Install TypeTreeGeneratorAPI into the Python selected by HKPSP_PYTHON for the full-resolution boss export.' }
}
# -Prebuilt (player bundle): EBOOT.PBP and hkpsp-host.exe built from this source
# tree, listed with their SHA-256 in prebuilt.json. No g++, make or WSL then.
if ($Prebuilt) {
    $Prebuilt = (Get-Item -LiteralPath $Prebuilt -ErrorAction Stop).FullName
    $prebuiltInfo = Get-Content -LiteralPath (Join-Path $Prebuilt 'prebuilt.json') -Raw | ConvertFrom-Json
    # The replay host is per OS: hkpsp-host.exe on Windows, hkpsp-host on Linux.
    $hostName = if ($IsWindows) { 'hkpsp-host.exe' } else { 'hkpsp-host' }
    foreach ($name in @('EBOOT.PBP', $hostName)) {
        $file = Join-Path $Prebuilt $name
        if (-not (Test-Path -LiteralPath $file -PathType Leaf) -or
            (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $prebuiltInfo.files.$name) {
            throw "The prebuilt $name is missing or damaged. Download the HKPSP bundle again."
        }
    }
} else {
$compiler = if ($env:HKPSP_HOST_COMPILER) { $env:HKPSP_HOST_COMPILER } else { Join-Path $repo '_private/tools/w64devkit/bin/g++.exe' }
$hostCompiler = Get-Command $compiler -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $hostCompiler) { throw 'Set HKPSP_HOST_COMPILER to your w64devkit g++.exe.' }
$make = if ($env:HKPSP_HOST_MAKE) { $env:HKPSP_HOST_MAKE } else { Join-Path (Split-Path -Parent $hostCompiler.Source) 'make.exe' }
if (-not (Get-Command $make -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1)) {
    throw 'Set HKPSP_HOST_MAKE to the make.exe paired with your host compiler.'
}
if (-not (Get-Command wsl.exe -CommandType Application -ErrorAction SilentlyContinue)) {
    throw 'Install the WSL PSP toolchain described in docs/psp/toolchain.md.'
}
}

# Report concrete inputs already required by the offline converters. Other
# scene-specific prerequisites are reported by the conversion graph itself.
$extractableExports = @('_private/research/assets/export-map.json', '_private/research/hud/hud-map.json',
                        '_private/research/inventory/inventory-map.json',
                        '_private/research/hud-layout/layout-map.json', '_private/research/hud-liquid/liquid-map.json',
                        '_private/research/hud-feedback/feedback-map.json', '_private/research/hud-limiter/limiter-map.json',
                        '_private/research/hero-death/death-map.json', '_private/research/hero-effects/hero-effects.json',
                        '_private/research/focus-effects/focus-effects.json', '_private/research/hero-light/level6-12820.json',
                        '_private/research/hero-light/fsm-23559.json', '_private/research/hero-light-donut/resources.assets-3318.json',
                        '_private/research/hero-damage-flash/damage-flash.json', '_private/research/hero-damage-flings/damage-flings.json',
                        '_private/research/terrain/terrain-map.json', '_private/research/tutorial/inventory.json',
                        '_private/research/room-entities/sharedassets6.assets-1150.json',
                        '_private/research/enemy/crawler-map.json', '_private/research/buzzer/buzzer-map.json',
                        '_private/research/room-visuals/room-visuals-map.json',
                        '_private/research/fury-mask/manifest.json', '_private/research/great-door/sources.json',
                        '_private/research/hollow-shade/shade-map.json', '_private/research/fury-chest/fury-chest-map.json',
                        '_private/research/ability-pickups/manifest.json', '_private/minimum-abilities/spell-source/art/fireball2-spiral-summary.json',
                        '_private/research/tutorial-reminders/prompt-map.json', '_private/research/opening-credits/credits-map.json',
                        '_private/research/lore-tablets/lore-map.json', '_private/research/tablet-glow/tablet-glow-map.json',
                        '_private/research/area-title/title-map.json', '_private/research/area-title/large-title-map.json')
$titleDir = Join-Path $repo '_private/research/title-menu'
$titleSourceExports = @('_private/research/title-menu/provenance.json',
                        '_private/research/title-menu/menu-particles.json')
$titleBackdrop = '_private/research/title-menu/menu-background-source.png'
$baseExports = $extractableExports + $titleSourceExports + @($titleBackdrop, '_private/research/title-menu/menu-arrow-source.png')
if ($Check) {
    "Steam input: $steam"
    "Python: $python"
    $checkExports = @()
    if (-not ($AssetRipperPort -or $AssetRipperExe)) {
        $checkExports += $extractableExports + $titleSourceExports
    }
    $missingExports = @($checkExports | Where-Object { -not (Test-Path -LiteralPath (Join-Path $repo $_) -PathType Leaf) })
    if ($missingExports.Count) { throw "Local exports still required: $($missingExports -join ', '). See README.md and tools/regen.ps1 -List." }
    if (-not $IlspyExe -and -not (Test-Path -LiteralPath (Join-Path $repo '_private/research/decompiled/all') -PathType Container)) {
        throw 'Install ilspycmd or pass -IlspyExe to prepare the managed source exports.'
    }
    if (-not $FreeTypeDll -and
        -not (Test-Path -LiteralPath (Join-Path $repo '_private/research/hud-geo-font/font-map.json') -PathType Leaf) -and
        -not (Test-Path -LiteralPath (Join-Path $repo '_private/research/hud-liquid-capture/font-map.json') -PathType Leaf)) {
        throw 'Pass -FreeTypeDll to rasterize the original Geo HUD font.'
    }
    'Base input check passed. Run the build to check the remaining converter inputs.'
    return
}

# A junction made by another Windows account is an untrusted mount point on Windows 11 24H2
# (laptop 2026-11-27): it cannot be traversed, the hash check below failed and the build stopped.
# When the link points at the same Steam folder, recreate only the link for this account.
$sameLink = $false
$existingLink = Get-Item -LiteralPath $archive -Force -ErrorAction SilentlyContinue
if ($SteamGame -and $existingLink -and $existingLink.LinkType -in @('Junction', 'SymbolicLink')) {
    $linkTarget = [IO.Path]::GetFullPath([string]@($existingLink.Target)[0]).TrimEnd('\', '/')
    if ($linkTarget -ieq $steam.TrimEnd('\', '/')) {
        $sameLink = $true
        try { Get-ChildItem -LiteralPath $archive -Force -ErrorAction Stop | Out-Null }
        catch {
            "Recreating the link to the Steam game for this account: $archive"
            $existingLink.Delete()
            New-Item -ItemType $(if ($IsWindows) { 'Junction' } else { 'SymbolicLink' }) -Path $archive -Target $steam | Out-Null
        }
    }
}
# Keep the original files read only. A junction is useful on a small build
# drive and allows the converters to retain their stable archive paths.
if ($UseInstalledFiles -and -not (Test-Path -LiteralPath $archive -PathType Container)) {
    if ($steam -eq $archive -or $steam.StartsWith($archive + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Select the Steam install outside the build archive directory.'
    }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $archive) | Out-Null
    $linkType = if ($IsWindows) { 'Junction' } else { 'SymbolicLink' }
    New-Item -ItemType $linkType -Path $archive -Target $steam | Out-Null
} elseif (-not (Test-Path -LiteralPath $archive -PathType Container)) {
    $inputPrefix = $steam.TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if ($archive.StartsWith($inputPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'The original copy cannot be created inside the Steam input folder.'
    }
    $parent = Split-Path -Parent $archive
    foreach ($path in @((Join-Path $repo '_private'), $parent)) {
        $item = Get-Item -LiteralPath $path -ErrorAction SilentlyContinue
        if ($item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Original copy must be inside the checkout: $path"
        }
    }
    $bytes = (Get-ChildItem -LiteralPath $steam -Recurse -File -Force | Measure-Object -Property Length -Sum).Sum
    $drive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($parent))
    if ($drive.AvailableFreeSpace -lt $bytes) {
        throw "The private copy needs $bytes bytes on $($drive.Name). Available space is $($drive.AvailableFreeSpace) bytes, before conversion."
    }
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    $pending = Join-Path $parent ('.steam-import-' + [guid]::NewGuid().ToString('N'))
    try {
        Copy-Item -LiteralPath $steam -Destination $pending -Recurse -Force
        $resolved = (Resolve-Path -LiteralPath $pending -ErrorAction Stop).Path
        if ($resolved -ne $pending) { throw "Unexpected original copy path: $resolved" }
        [IO.Directory]::Move($resolved, $archive)
    } finally {
        if (Test-Path -LiteralPath $pending) {
            $leftover = Get-Item -LiteralPath $pending -Force
            if ($leftover.FullName -eq $pending -and -not ($leftover.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
                Remove-Item -LiteralPath $pending -Recurse -Force
            }
        }
    }
} elseif ($SteamGame -and -not $sameLink) {
    foreach ($file in $required) {
        $installed = Join-Path $gameData $file
        $copied = Join-Path $archive "hollow_knight_Data/$file"
        if (-not (Test-Path -LiteralPath $copied -PathType Leaf) -or
            (Get-FileHash -LiteralPath $installed).Hash -ne (Get-FileHash -LiteralPath $copied).Hash) {
            throw "The local original copy differs from -SteamGame at $file. Use a clean input tree for a different Steam build."
        }
    }
}
$logs = Join-Path $repo 'build/psp/steam-build'
$archiveManifest = Join-Path (Split-Path -Parent $archive) 'appmanifest_367520.acf'
if (-not (Test-Path -LiteralPath $archiveManifest -PathType Leaf)) {
    Copy-Item -LiteralPath $steamManifest -Destination $archiveManifest
}
if ($IlspyExe) {
    New-Item -ItemType Directory -Force -Path $logs | Out-Null
    $log = Join-Path $logs 'managed-exports.log'
    "Starting export managed-exports. Log at $log"
    & $python -I (Join-Path $PSScriptRoot 'export_steam_managed.py') --ilspy $IlspyExe --assembly (Join-Path $archive 'hollow_knight_Data/Managed/Assembly-CSharp.dll') --output (Join-Path $repo '_private/research/decompiled') *> $log
    if ($LASTEXITCODE) {
        Get-Content -LiteralPath $log -Tail 12
        throw "Managed source export failed. See $log."
    }
}
if ($AssetRipperPort -or $AssetRipperExe) {
    New-Item -ItemType Directory -Force -Path $logs | Out-Null
    $log = Join-Path $logs 'base-exports.log'
    "Starting export base-exports. Log at $log"
    $exportArgs = if ($AssetRipperExe) { @('--assetripper', $AssetRipperExe, '--game-data', (Join-Path $archive 'hollow_knight_Data')) } else { @('--port', $AssetRipperPort) }
    $exportArgs += '--title-source'
    & $python -I (Join-Path $PSScriptRoot 'export_steam_base.py') @exportArgs *> $log
    if ($LASTEXITCODE) {
        Get-Content -LiteralPath $log -Tail 12
        throw "Base export failed. See $log."
    }
}
if ($FreeTypeDll) {
    New-Item -ItemType Directory -Force -Path $logs | Out-Null
    $log = Join-Path $logs 'geo-font.log'
    "Starting export geo-font. Log at $log"
    & $python -I (Join-Path $PSScriptRoot 'export_steam_geo_font.py') --freetype $FreeTypeDll --research (Join-Path $repo '_private/research') *> $log
    if ($LASTEXITCODE) {
        Get-Content -LiteralPath $log -Tail 12
        throw "Geo font export failed. See $log."
    }
}
if ((Test-Path -LiteralPath (Join-Path $titleDir 'provenance.json') -PathType Leaf) -and
    -not (Test-Path -LiteralPath (Join-Path $titleDir 'menu-arrow-source.png') -PathType Leaf)) {
    & $python -I (Join-Path $repo 'tools/convert_title_menu.py') --extract-menu-arrow --source $titleDir --original (Join-Path $archive 'hollow_knight_Data')
    if ($LASTEXITCODE) { throw 'Menu flourish extraction failed.' }
}
if ($Cecil) {
    New-Item -ItemType Directory -Force -Path $logs | Out-Null
    $log = Join-Path $logs 'title-capture.log'
    "Starting export title-capture. Log at $log"
    & $python -I (Join-Path $PSScriptRoot 'capture_steam_title.py') --steam-manifest $steamManifest --dotnet $DotnetRoot --cecil $Cecil *> $log
    if ($LASTEXITCODE) {
        Get-Content -LiteralPath $log -Tail 12
        throw "Title capture failed. See $log."
    }
} elseif (Test-Path -LiteralPath (Join-Path $titleDir 'provenance.json') -PathType Leaf) {
    New-Item -ItemType Directory -Force -Path $logs | Out-Null
    $log = Join-Path $logs 'title-backdrop.log'
    "Starting export title-backdrop. Log at $log"
    & $python -I (Join-Path $PSScriptRoot 'render_steam_title.py') --source $titleDir --original (Join-Path $archive 'hollow_knight_Data') *> $log
    if ($LASTEXITCODE) {
        Get-Content -LiteralPath $log -Tail 12
        throw "Offline title render failed. See $log."
    }
}
if ($Route -and $AssetRipperExe) {
    New-Item -ItemType Directory -Force -Path $logs | Out-Null
    $log = Join-Path $logs 'route-exports.log'
    "Starting export route-exports. Log at $log"
    $routeArgs = @('--assetripper', $AssetRipperExe, '--game-data', (Join-Path $archive 'hollow_knight_Data'), '--jobs', $Jobs)
    if ($AllRooms) { $routeArgs += '--all-rooms' }
    & $python -I (Join-Path $PSScriptRoot 'export_steam_route.py') @routeArgs *> $log
    if ($LASTEXITCODE) {
        Get-Content -LiteralPath $log -Tail 12
        throw "Route export failed. See $log."
    }
    # Boss, enemy, obstacle and effect sources the converters read beyond the
    # room scenes. Finished jobs are stamped, so a rerun resumes.
    $log = Join-Path $logs 'extra-exports.log'
    "Starting export extra-exports. Log at $log"
    & $python -I (Join-Path $PSScriptRoot 'export_steam_extras.py') --assetripper $AssetRipperExe --game-data (Join-Path $archive 'hollow_knight_Data') *> $log
    if ($LASTEXITCODE) {
        Get-Content -LiteralPath $log -Tail 12
        throw "Extra source export failed. See $log and build/psp/steam-build/extras.log."
    }
}
$missingExports = @($baseExports | Where-Object { -not (Test-Path -LiteralPath (Join-Path $repo $_) -PathType Leaf) })
if ($missingExports.Count) {
    throw "Steam files are ready. Local exports still required: $($missingExports -join ', '). See README.md and tools/regen.ps1 -List."
}

$oldSource = $env:HKPSP_SOURCE_ROOT
$oldPython = $env:HKPSP_PYTHON
$oldRegenPython = $env:HK3DS_PYTHON
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$pwsh = Join-Path $PSHOME $(if ($IsWindows) { 'pwsh.exe' } else { 'pwsh' })
Push-Location $repo
try {
    $env:HKPSP_SOURCE_ROOT = $repo
    $env:HKPSP_PYTHON = $python
    $env:HK3DS_PYTHON = $python  # The shared resource graph still uses this variable name.
    # The converter scheduler starts a job only with this much RAM free (developer default 4 GB).
    # A 16 GB laptop with a browser open had 2.8 GB free and waited forever without a word
    # (2026-11-27). Player builds run one job at a time down to 1 GB free.
    $oldMinFree = $env:HK3DS_WRITE_MIN_FREE_GB
    # Count the page file too (commit headroom): with 2.5 GB of free RAM the physical floor let
    # only 1-2 of 6 converters run while the CPU sat at 1% (laptop 2026-11-27).
    if (-not $oldMinFree) { $env:HK3DS_WRITE_MIN_FREE_GB = 'commit:2' }
    # Parallel converters: one per logical core, but at most one per 2 GB of RAM so a weak PC is
    # not overloaded. Measured peaks (laptop 2026-11-27): scene converters up to 575 MB, average
    # 163 MB; the developer default (half the cores) left a 12-core laptop at 29% CPU.
    $oldJobs = $env:HK3DS_REGEN_JOBS
    if (-not $oldJobs) {
        $ramGb = if ($IsWindows) { (Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB } else {
            [double]((Get-Content /proc/meminfo | Select-String '^MemTotal:\s+(\d+)').Matches[0].Groups[1].Value) / 1MB }
        $env:HK3DS_REGEN_JOBS = [string][Math]::Max(1, [Math]::Min([Environment]::ProcessorCount, [Math]::Floor($ramGb / 2)))
        "Converters run $($env:HK3DS_REGEN_JOBS) at a time ($([Environment]::ProcessorCount) cores, $([Math]::Round($ramGb)) GB RAM)."
    }
    $steps = [ordered]@{
        resources = @('tools/regen.ps1', '-Python', $python)
        # The measured Tutorial_01 repack recipe fits the prepared tables only.
        psp = @('tools/psp/regen-assets.ps1', '-Only', 'stale', '-AllowUnpackedTutorial')
    }
    if ($Route -and -not $AllRooms) {
        $steps['resources'] += '-PspRoute'
        $steps['rooms'] = @('tools/psp/regen-assets.ps1', '-Only', 'routerooms')
    }
    $steps['package'] = @('tools/psp/package.ps1', '-GameOnly')
    if ($Prebuilt) { $steps['package'] += @('-Eboot', (Join-Path $Prebuilt 'EBOOT.PBP'), '-HostExe', (Join-Path $Prebuilt $hostName)) }
    if ($AllRooms) {
        # package.ps1 converts and stages the explicit room list itself.
        $roomList = @(& $python -I (Join-Path $PSScriptRoot 'export_steam_route.py') --list-rooms)
        if ($LASTEXITCODE -or -not $roomList.Count) { throw 'Could not list the PSP rooms.' }
        # Tutorial_01 is the main room package, never a scene package (room_packages.py
        # requires scenes/<room>/scene.bin, which King's Pass does not have).
        $steps['package'] += @('-Rooms', (@($roomList | Where-Object { $_ -ne 'Tutorial_01' }) -join ','))
    } elseif ($Route) { $steps['package'] += '-Route' }
    foreach ($step in $steps.GetEnumerator()) {
        if ($step.Key -eq 'package' -and $Route -and -not $AllRooms) {
            $routeIndex = Join-Path $repo '_private/converted-psp/rooms.json'
            $routePlan = Get-Content -LiteralPath $routeIndex -Raw | ConvertFrom-Json
            # The census excludes only scenes it marks as not yet ported (status U)
            # and raises for any other missing conversion, so a failed export
            # still stops the build. Release builds ship the rooms that exist
            # and say which route scenes the game does not have yet.
            if ($routePlan.excluded) {
                "Not in this release yet: $($routePlan.excluded -join ', '). The route stops before these scenes."
            }
        }
        $log = Join-Path $logs "$($step.Key).log"
        "Starting $($step.Key). Log at $log"
        $arguments = $step.Value
        & $pwsh -NoProfile -File @arguments *> $log
        if ($LASTEXITCODE) {
            Get-Content -LiteralPath $log -Tail 12
            throw "Build step $($step.Key) failed. See $log."
        }
    }
    "Game package: $(Join-Path $repo 'build/psp/package-game/PSP/GAME/HKPSP')"
} finally {
    $env:HKPSP_SOURCE_ROOT = $oldSource
    $env:HKPSP_PYTHON = $oldPython
    $env:HK3DS_PYTHON = $oldRegenPython
    $env:HK3DS_WRITE_MIN_FREE_GB = $oldMinFree
    $env:HK3DS_REGEN_JOBS = $oldJobs
    Pop-Location
}
