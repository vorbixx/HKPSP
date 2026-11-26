function Get-HkpspSourceRoot([string]$Repo) {
    if ($env:HKPSP_SOURCE_ROOT) { return [IO.Path]::GetFullPath($env:HKPSP_SOURCE_ROOT) }
    if ((Test-Path -LiteralPath (Join-Path $Repo '_private/original/Hollow Knight/hollow_knight_Data') -PathType Container) -or
        (Test-Path -LiteralPath (Join-Path $Repo '_private/converted/hero/romfs/tutorial-room.bin') -PathType Leaf)) {
        return $Repo
    }
    # Existing development worktrees can still consume the prepared sibling export.
    foreach ($relative in '../HK3DS', '../../HK3DS') {
        $candidate = [IO.Path]::GetFullPath((Join-Path $Repo $relative))
        if (Test-Path -LiteralPath $candidate -PathType Container) { return $candidate }
    }
    return $Repo
}

function Get-HkpspPython {
    if ($env:HKPSP_PYTHON) { return (Get-Command $env:HKPSP_PYTHON -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source }
    $launcher = Get-Command py.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($launcher) {
        $executable = & $launcher.Source -3.12 -I -c 'import sys; print(sys.executable)' 2>$null
        if ($LASTEXITCODE -eq 0 -and $executable) { return $executable }
    }
    foreach ($candidate in Get-Command python.exe -CommandType Application -ErrorAction SilentlyContinue) {
        & $candidate.Source -I -c 'import sys; sys.exit(sys.version_info[:2] != (3, 12))' 2>$null
        if ($LASTEXITCODE -eq 0) { return $candidate.Source }
    }
    throw 'Python 3.12 was not found. Set HKPSP_PYTHON to its python.exe.'
}
