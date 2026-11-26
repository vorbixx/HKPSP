param(
    [switch]$Run,
    [ValidateRange(1,1000000)][int]$Frames=300,
    [ValidateRange(1,1000000)][int]$Every=1,
    [switch]$Fast,
    [switch]$Gpu,
    [switch]$Cpu,
    [switch]$PageTrace,
    [switch]$SimTrace,
    # Player bundle: run this prebuilt replay host instead of compiling one (needs -Run, normal build).
    [string]$HostExe
)
$ErrorActionPreference='Stop'
$repoRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. (Join-Path $PSScriptRoot 'build-config.ps1')
$sourceRoot=Get-HkpspSourceRoot $repoRoot
$toolBin=Join-Path $sourceRoot '_private/tools/w64devkit/bin'
# A worktree (C:/Projects/HKPSP-wt/<name>) has no ../HK3DS sibling; its own _private/tools junction
# leads to the same read-only toolchain. Use it when HKPSP_SOURCE_ROOT is not set.
if (-not $env:HKPSP_SOURCE_ROOT -and -not (Test-Path -LiteralPath (Join-Path $toolBin 'g++.exe')) -and
    (Test-Path -LiteralPath (Join-Path $repoRoot '_private/tools/w64devkit/bin/g++.exe'))) {
    $toolBin=Join-Path $repoRoot '_private/tools/w64devkit/bin'
}
$compiler=if ($env:HKPSP_HOST_COMPILER) { (Get-Command $env:HKPSP_HOST_COMPILER -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source } else { Join-Path $toolBin 'g++.exe' }
$make=if ($env:HKPSP_HOST_MAKE) { (Get-Command $env:HKPSP_HOST_MAKE -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source } else { Join-Path (Split-Path -Parent $compiler) 'make.exe' }
$toolBin=Split-Path -Parent $compiler
if ($HostExe) {
    if (-not $Run -or $Gpu -or $Cpu -or $PageTrace -or $SimTrace) { throw '-HostExe runs the prebuilt normal replay host only (-Run)' }
    $HostExe=(Get-Item -LiteralPath $HostExe -ErrorAction Stop).FullName
} elseif (-not (Test-Path -LiteralPath $compiler)) { throw "Missing compiler: $compiler. Set HKPSP_HOST_COMPILER to your w64devkit g++.exe." }
if ($Gpu -and $Cpu) { throw 'Gpu and Cpu are separate host builds' }
if ($PageTrace -and ($Gpu -or $Cpu)) { throw 'PageTrace requires the normal replay build' }
if ($SimTrace -and ($Gpu -or $Cpu -or $PageTrace)) { throw 'SimTrace requires the normal replay build' }
$outputRelative=if ($SimTrace) { 'build/psp/sim-host' } elseif ($PageTrace) { 'build/psp/page-host' } elseif ($Gpu) { 'build/psp/gpu-host' } elseif ($Cpu) { 'build/psp/cpu-host' } else { 'build/psp/host' }
# Linux (pwsh on Linux, e.g. the Linux release prebuilt): own output folder and no .exe, so
# Windows and Linux objects never mix in one tree. -Static links a host that runs on any distro.
# Linux warnings stay warnings (-Wno-error): GCC 13 + glibc report a -Wrestrict false positive
# (GCC PR105651) and ignored fclose attributes in unique_ptr deleters. The Windows host build
# keeps -Werror as the gate.
$exeName='hkpsp-host.exe'
if (-not ($IsWindows -or $PSVersionTable.PSEdition -eq 'Desktop')) { $outputRelative+='-linux'; $exeName='hkpsp-host' }
$output=Join-Path $repoRoot $outputRelative
if ($Gpu) { $Frames=150; $Every=150; $Fast=$true }
if ($Cpu) { $Frames=600; $Every=600; $Fast=$true }
if ($Cpu -and (Test-Path (Join-Path $output 'quad-parity.txt'))) { Remove-Item -LiteralPath (Join-Path $output 'quad-parity.txt') }
New-Item -ItemType Directory -Force (Join-Path $output 'obj') | Out-Null
$modules=@()
# CORE is the shared host's authoritative platform-free set, including += lines.
foreach ($line in Get-Content (Join-Path $repoRoot 'tools/host.mk')) {
    if ($line -match '^CORE\s*(:=|\+=)\s*(.*)$') {
        if ($Matches[1] -eq ':=') { $modules=@() }
        $modules+=@(($Matches[2] -replace '#.*$','').Trim() -split '\s+' | Where-Object { $_ })
    }
}
if (-not $modules.Count) { throw 'No CORE modules found in tools/host.mk' }
$sources=@($modules | Select-Object -Unique | ForEach-Object { "src/$_.cpp" })
$sources+=@(Get-ChildItem (Join-Path $repoRoot 'psp/src') -Filter '*.cpp' |
    Where-Object { $_.Name -ne 'backend_psp.cpp' } | Sort-Object Name | ForEach-Object { "psp/src/$($_.Name)" })
$objects=@($sources | ForEach-Object { "$outputRelative/obj/"+($_ -replace '[/\\]','_' -replace '\.cpp$','.o') })
$lines=@('.DEFAULT_GOAL := all','.DELETE_ON_ERROR:',
    ('CXX := '+($compiler -replace '\\','/')),
    ('CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Werror -pedantic -Iinclude -Ipsp/include'+$(if (-not ($IsWindows -or $PSVersionTable.PSEdition -eq 'Desktop')) { ' -Wno-error' })+$(if ($Gpu) { ' -DHKPSP_GPU_ATTRIB' })+$(if ($Cpu) { ' -DHKPSP_CPU_ATTRIB' })+$(if ($PageTrace) { ' -DHKPSP_PAGE_TRACE' })+$(if ($SimTrace) { ' -DHKPSP_SIM_TRACE' })),
    ('OBJECTS := '+($objects -join ' ')),
    ("all: $outputRelative/$exeName"),
    ("$outputRelative/$exeName"+': $(OBJECTS)'),
    $(if ($IsWindows -or $PSVersionTable.PSEdition -eq 'Desktop') { "`t`"`$(CXX)`" `$(OBJECTS) -Wl,--no-insert-timestamp -o `$@" } else { "`t`"`$(CXX)`" `$(OBJECTS) -static -o `$@" }))
for ($i=0;$i -lt $sources.Count;$i++) {
    $lines+=($objects[$i]+': '+$sources[$i]+" $outputRelative/host.mk")
    $lines+="`t@echo CXX $($sources[$i])"
    $lines+="`t@`"`$(CXX)`" `$(CXXFLAGS) -MMD -MP -c `$< -o `$@"
}
$lines+='-include $(OBJECTS:.o=.d)'
$content=($lines -join "`n")+"`n"
$makefile=Join-Path $output 'host.mk'
if (-not (Test-Path $makefile) -or [IO.File]::ReadAllText($makefile) -cne $content) {
    [IO.File]::WriteAllText($makefile,$content,[Text.UTF8Encoding]::new($false))
}
$previousPath=$env:PATH
Push-Location $repoRoot
try {
    $env:PATH=$toolBin+[IO.Path]::PathSeparator+$previousPath
    $hostProgram=Join-Path $output $exeName
    if ($HostExe) {
        $hostProgram=$HostExe
        Write-Output "Using the prebuilt replay host $HostExe"
    } else {
        & $make -f "$outputRelative/host.mk" -j4
        if ($LASTEXITCODE -ne 0) { throw "Host build failed: $LASTEXITCODE" }
        Write-Output "Built $($modules.Count) CORE modules + $($sources.Count-$modules.Count) PSP sources"
    }
    if ($Run) {
        $arguments=@('--assets',(Join-Path $repoRoot '_private/converted-psp'),'--source',$sourceRoot,
            '--output',$output,'--frames',"$Frames",'--every',"$Every")
        if ($Fast) { $arguments+='--fast' }
        & $hostProgram @arguments
        if ($LASTEXITCODE -ne 0) { throw "Host run failed: $LASTEXITCODE" }
        if (-not ($Gpu -or $Cpu -or $PageTrace -or $SimTrace)) {
            # Ordinary play boots the title. Trace an actual title frame as well
            # as gameplay so staging remains limited to successfully read assets.
            $menuOutput=Join-Path $output 'menus'
            & $hostProgram --assets (Join-Path $repoRoot '_private/converted-psp') --source $sourceRoot --output $menuOutput --menus --frames 1 --fast --no-raster
            if ($LASTEXITCODE -ne 0) { throw "Title asset trace failed: $LASTEXITCODE" }
            $trace=Join-Path $output 'assets.tsv'
            $menuTrace=Join-Path $menuOutput 'assets.tsv'
            $rows=@(Get-Content -LiteralPath $trace)+@(Get-Content -LiteralPath $menuTrace)
            [IO.File]::WriteAllLines($trace, @($rows | Select-Object -Unique), [Text.UTF8Encoding]::new($false))
        }
    }
} finally {
    Pop-Location
    $env:PATH=$previousPath
}
