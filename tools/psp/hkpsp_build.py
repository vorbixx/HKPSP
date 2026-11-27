"""One-step HKPSP game builder for players (docs/tasks/psp-user-builder.md).

Finds the player's Steam copy of Hollow Knight, checks the version, disk space
and work path before any work, then runs the build. `--check` stops after the
checks. Every failure ends with one message saying what to do.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

APP_ID = '367520'
GAME_FOLDER = 'Hollow Knight'
# Steam build IDs whose files the converters were verified on.
SUPPORTED_BUILDS = {
    '22529139': 'Steam 1.5.12620 (Windows)',
}
# Files the converters read; a copy without them is not a complete install.
REQUIRED_FILES = ('globalgamemanagers.assets', 'resources.assets', 'resources.assets.resS',
                  'Managed/Assembly-CSharp.dll', 'level1', 'level6', 'sharedassets0.assets',
                  'sharedassets1.assets', 'sharedassets1.assets.resS')
# [H] Full build with the developer cache measured about 40 GB (2026-11-25).
# Player mode (no regen cache, 3DS textures cut to their header, the Steam install read in place):
# 2026-11-25 developer build measured 56 GB unique, of it 13 GB cache history and 17 GB .t3x;
# about 26 GB stays [H, to be measured in an end-to-end bundle run], plus headroom.
REQUIRED_FREE_GB = 32
# Work files a player build leaves; removed after success unless --keep. Everything else in
# _private/ and build/psp/ is kept: the bundled tools and the finished package.
KEEP_PRIVATE = {'tools'}
KEEP_BUILD_PSP = {'package-game', 'steam-build'}


class BuildError(Exception):
    """A problem the player can fix; the message says how."""


def parse_vdf(text):
    """Parse Valve KeyValues text (libraryfolders.vdf, appmanifest_*.acf)."""
    tokens = re.findall(r'"((?:[^"\\]|\\.)*)"|([{}])', text)
    stack = [{}]
    key = None
    for quoted, brace in tokens:
        if brace == '{':
            if key is None:
                raise ValueError('VDF block without a key')
            child = {}
            stack[-1][key] = child
            stack.append(child)
            key = None
        elif brace == '}':
            if len(stack) == 1:
                raise ValueError('Unbalanced VDF braces')
            stack.pop()
        elif key is None:
            key = quoted.replace('\\\\', '\\')
        else:
            stack[-1][key] = quoted.replace('\\\\', '\\')
            key = None
    if len(stack) != 1:
        raise ValueError('Unclosed VDF block')
    return stack[0]


def steam_roots(platform=sys.platform, home=None, env=None):
    """Steam installation folders to search, most likely first."""
    env = os.environ if env is None else env
    home = Path(home or Path.home())
    roots = []
    if platform.startswith('win'):
        try:
            import winreg
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r'Software\Valve\Steam') as key:
                roots.append(Path(winreg.QueryValueEx(key, 'SteamPath')[0]))
        except OSError:
            pass
        for variable in ('ProgramFiles(x86)', 'ProgramFiles'):
            if env.get(variable):
                roots.append(Path(env[variable]) / 'Steam')
    else:
        roots += [home / '.steam/steam', home / '.local/share/Steam', home / '.steam/root',
                  home / '.var/app/com.valvesoftware.Steam/.local/share/Steam',
                  home / 'snap/steam/common/.local/share/Steam']
        if platform == 'darwin':
            roots.append(home / 'Library/Application Support/Steam')
    unique = []
    for root in roots:
        if root not in unique:
            unique.append(root)
    return unique


def steam_libraries(root):
    """steamapps folders of one Steam installation, including extra libraries."""
    libraries = [root / 'steamapps']
    vdf = root / 'steamapps/libraryfolders.vdf'
    if vdf.is_file():
        data = parse_vdf(vdf.read_text(encoding='utf-8', errors='replace'))
        for entry in data.get('libraryfolders', {}).values():
            path = entry.get('path') if isinstance(entry, dict) else entry
            if path:
                libraries.append(Path(path) / 'steamapps')
    return libraries


def find_installs(roots):
    """(game folder, appmanifest) for every Steam library that has the game."""
    found = []
    for root in roots:
        for library in steam_libraries(root) if root.is_dir() else []:
            manifest = library / f'appmanifest_{APP_ID}.acf'
            if not manifest.is_file():
                continue
            state = parse_vdf(manifest.read_text(encoding='utf-8', errors='replace')).get('AppState', {})
            game = library / 'common' / state.get('installdir', GAME_FOLDER)
            if game.is_dir() and (game, manifest) not in found:
                found.append((game, manifest))
    return found


def manifest_for(game):
    """appmanifest next to a game folder given by hand (steamapps/common/<game>)."""
    for folder in (game.parent.parent, game.parent):
        candidate = folder / f'appmanifest_{APP_ID}.acf'
        if candidate.is_file():
            return candidate
    return None


def game_data(game):
    """The Unity data folder; Linux and Windows builds use the same name."""
    return game / 'hollow_knight_Data'


def check_game(game, manifest):
    """Return (build id, label) or raise with what the player should do."""
    data = game_data(game)
    missing = [name for name in REQUIRED_FILES if not (data / name).is_file()]
    if missing:
        raise BuildError(f'The game folder {game} is incomplete (missing {", ".join(missing)}). '
                         'In Steam use Properties > Installed Files > Verify integrity, then run again.')
    if manifest is None:
        raise BuildError(f'No Steam appmanifest_{APP_ID}.acf found next to {game}. '
                         'Select the folder Steam opens with Manage > Browse local files.')
    state = parse_vdf(manifest.read_text(encoding='utf-8', errors='replace')).get('AppState', {})
    build = state.get('buildid', '')
    if build not in SUPPORTED_BUILDS:
        supported = ', '.join(f'{b} ({label})' for b, label in SUPPORTED_BUILDS.items())
        raise BuildError(f'This Hollow Knight copy is Steam build {build or "unknown"}; the builder supports '
                         f'{supported}. Update the game in Steam (no beta branch) or use a newer HKPSP release.')
    return build, SUPPORTED_BUILDS[build]


def work_data_gb(work):
    """GB already written by an earlier run (_private, build/psp); links into the game are skipped.
    os.scandir entries carry their size on Windows, so no extra stat call per file."""
    total, stack = 0, [work / '_private', work / 'build/psp']
    while stack:
        try:
            entries = os.scandir(stack.pop())
        except OSError:
            continue
        with entries:
            for entry in entries:
                try:
                    if entry.is_symlink() or entry.is_junction():
                        continue
                    if entry.is_dir(follow_symlinks=False):
                        stack.append(entry.path)
                    else:
                        total += entry.stat(follow_symlinks=False).st_size
                except OSError:
                    pass
    return total / 2**30


def check_work_dir(work, required_gb=REQUIRED_FREE_GB):
    """Return warnings; raise when the build cannot fit."""
    work.mkdir(parents=True, exist_ok=True)
    probe = work / '.write-test'
    try:
        probe.write_bytes(b'ok')
        probe.unlink()
    except OSError as error:
        raise BuildError(f'Cannot write to {work} ({error}). Unpack HKPSP in a folder you can write to.')
    free = shutil.disk_usage(work).free / 2**30
    # A resumed build already holds part of its data; only the rest must still fit.
    required_gb = max(4.0, required_gb - work_data_gb(work))
    if free < required_gb:
        raise BuildError(f'The build needs about {required_gb} GB free in {work}; {free:.1f} GB is free. '
                         'Free some space, or unpack HKPSP on another drive and run it from there.')
    warnings = []
    if not str(work.resolve()).isascii():
        warnings.append(f'The work folder path {work} has non-English letters. If a step fails, '
                        'move the HKPSP folder to a plain path such as C:\\HKPSP.')
    return warnings


# Third-party tools inside the release bundle (docs/tasks/psp-user-builder.md, decision 3.1).
# bundle.json at the bundle root maps each name to a path relative to that root; developers pass
# their own copy with --tools. Required on Windows today; Linux steps come later.
TOOLS = {
    'pwsh': 'PowerShell 7 (runs the build steps)',
    'python': 'Python 3.12 with the pinned packages',
    'assetripper': 'AssetRipper (exports the game files)',
    'ilspy': 'ilspycmd (reads the game code)',
    'freetype': 'FreeType library (renders the game fonts)',
}


def load_tools(path):
    """Tool name -> absolute path from a bundle.json; raise with what to do when one is missing."""
    if not path.is_file():
        raise BuildError(f'{path} is missing. Download the HKPSP release again and unpack all of it.')
    try:
        entries = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, ValueError) as error:
        raise BuildError(f'{path} is damaged ({error}). Download the HKPSP release again.')
    tools, missing = {}, []
    for name, label in TOOLS.items():
        value = entries.get(name)
        if value and value.startswith('system:'):
            # Linux: a library every desktop system has (FreeType); found, not shipped.
            tool = find_system_library(value.removeprefix('system:'))
            if tool is None:
                missing.append(f'{label}: install your system package for {value.removeprefix("system:")} '
                               '(Debian/Ubuntu libfreetype6, Fedora freetype, Arch freetype2)')
                continue
        else:
            tool = (path.parent / value).resolve() if value else None
        if tool is None or not tool.is_file():
            missing.append(f'{label} ({value or "not listed"})')
        else:
            tools[name] = tool
    # Prebuilt EBOOT and replay host (player bundle); a developer copy builds its own.
    if entries.get('prebuilt'):
        folder = (path.parent / entries['prebuilt']).resolve()
        if (folder / 'prebuilt.json').is_file():
            tools['prebuilt'] = folder
        else:
            missing.append(f"the prebuilt game program ({entries['prebuilt']})")
    if missing:
        raise BuildError('The HKPSP release is incomplete: ' + '; '.join(missing) +
                         '. Download it again and unpack all of it; some antivirus programs remove '
                         'tools, so check its quarantine.')
    return tools


LIBRARY_DIRS = ('/usr/lib/x86_64-linux-gnu', '/lib/x86_64-linux-gnu', '/usr/lib64', '/usr/lib', '/lib64', '/lib')


def find_system_library(name, ldconfig_text=None, dirs=LIBRARY_DIRS):
    """Path of a shared library of this Linux system, or None."""
    if ldconfig_text is None:
        try:
            ldconfig_text = subprocess.run(['ldconfig', '-p'], capture_output=True, text=True).stdout
        except OSError:
            ldconfig_text = ''
    for line in ldconfig_text.splitlines():
        parts = line.strip().split(' => ')
        if len(parts) == 2 and parts[0].split(' ')[0] == name and 'x86-64' in parts[0] and Path(parts[1]).is_file():
            return Path(parts[1])
    for folder in dirs:
        if (Path(folder) / name).is_file():
            return Path(folder) / name
    return None


def bundle_env(bundle_file, tools, base=None):
    """Environment for the build: bundled Python, tex3ds and .NET runtime (ilspycmd is
    framework-dependent) when bundle.json lists them. Player mode: no shared regen cache (its
    history was 13 GB of a developer build) and header-only 3DS textures (tools/tex3ds_cache.py)."""
    env = dict(os.environ if base is None else base, HKPSP_PYTHON=str(tools['python']),
               HK3DS_REGEN_CACHE='0', HKPSP_TRIM_3DS_TEXTURES='1',
               HKPSP_FAST_CHECKPOINT='1',
               HKPSP_STUB_3DS_TEXTURES='1')
    try:
        entries = json.loads(bundle_file.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        entries = {}
    if entries.get('tex3ds'):
        env['HKPSP_TEX3DS'] = (bundle_file.parent / entries['tex3ds']).resolve().as_posix()
    # Steps find these by name on PATH: ffmpeg (transcode_audio.py and four converters' fallback)
    # and pwsh (regen-assets.ps1, stage-data.ps1, extract_tutorial_prompts.py).
    folders = [str(tools['pwsh'].parent)]
    if entries.get('ffmpeg') and not entries['ffmpeg'].startswith('system:'):
        folders.insert(0, str((bundle_file.parent / entries['ffmpeg']).resolve().parent))
    env['PATH'] = os.pathsep.join(folders + [env.get('PATH', '')])
    if entries.get('dotnet-runtime'):
        env['DOTNET_ROOT'] = str((bundle_file.parent / entries['dotnet-runtime']).resolve().parent)
    return env


def build_command(tools, game, root=ROOT):
    """The build-steam.ps1 call for a full game build with the bundled tools."""
    return [str(tools['pwsh']), '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
            str(root / 'tools/psp/build-steam.ps1'), '-SteamGame', str(game), '-UseInstalledFiles', '-AllRooms',
            '-AssetRipperExe', str(tools['assetripper']), '-IlspyExe', str(tools['ilspy']),
            '-FreeTypeDll', str(tools['freetype'])] + (
            ['-Prebuilt', str(tools['prebuilt'])] if 'prebuilt' in tools else [])


# Lines worth showing the player; everything goes to the log.
PROGRESS = re.compile(r'^(Starting \w+|\[\d+/\d+\]|Game package:|ERROR|Exception)')


def heartbeat_line(started, folder, now=None, skip=''):
    """One reassuring line: elapsed time and the newest step log, so a silent step looks alive."""
    now = time.time() if now is None else now
    minutes = int(now - started) // 60
    line = f'Still working ({minutes // 60} h {minutes % 60:02d} min).'
    folders = folder if isinstance(folder, (list, tuple)) else [folder]
    logs = [p for f in folders for p in Path(f).glob('*.log') if p.is_file() and p.name != skip]
    if logs:
        newest = max(logs, key=lambda p: p.stat().st_mtime)
        idle = int(now - newest.stat().st_mtime) // 60
        line += f' Latest log: {newest.name}, updated {idle} min ago.'
    return line


def run_build(command, log_path, env, echo=print, heartbeat=300):
    """Run the build, keep the full output in log_path, show progress lines; return the exit code.

    Some steps (the AssetRipper exports) print nothing for an hour, so every `heartbeat`
    seconds a line shows the elapsed time and the newest step log (players 2026-11-26)."""
    log_path.parent.mkdir(parents=True, exist_ok=True)
    started, done = time.time(), threading.Event()

    def beat():
        while not done.wait(heartbeat):
            # The resources step writes one log per converter under build/regen/logs (laptop 2026-11-27:
            # steam-build looked idle for 20 min while converters ran).
            echo(heartbeat_line(started, [log_path.parent, ROOT / 'build/regen/logs'], skip=log_path.name))

    with open(log_path, 'w', encoding='utf-8', errors='replace') as log:
        with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              env=env, text=True, encoding='utf-8', errors='replace') as process:
            ticker = threading.Thread(target=beat, daemon=True)
            ticker.start()
            try:
                for line in process.stdout:
                    log.write(line)
                    log.flush()
                    if PROGRESS.match(line.strip()):
                        echo(line.rstrip())
            finally:
                done.set()
                ticker.join()
        return process.returncode


def support_zip(target, files, summary):
    """One zip to attach to a bug report: the summary plus the given logs that exist."""
    with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED) as archive:
        archive.writestr('summary.txt', summary)
        for path in files:
            if path.is_file():
                archive.write(path, path.name)
    return target


def is_link(path):
    """Symlink or Windows junction: removed as a link, never followed."""
    return path.is_symlink() or (hasattr(os.path, 'isjunction') and os.path.isjunction(path))


def remove_tree(path):
    """Delete a folder without following links inside it (the Steam game is reached through a
    junction in _private/original); return the bytes freed."""
    if is_link(path):
        os.rmdir(path) if os.name == 'nt' and path.is_dir() else path.unlink()
        return 0
    if not path.is_dir():
        size = path.stat().st_size
        path.unlink()
        return size
    freed = 0
    with os.scandir(path) as entries:
        for entry in entries:
            freed += remove_tree(Path(entry.path))
    path.rmdir()
    return freed


def clean_work(root=ROOT):
    """Remove the player build's work files, keep the package and logs; return bytes freed."""
    freed = 0
    for folder, keep in ((root / '_private', KEEP_PRIVATE), (root / 'build/psp', KEEP_BUILD_PSP)):
        if folder.is_dir() and not is_link(folder):
            for item in sorted(folder.iterdir()):
                if item.name not in keep:
                    freed += remove_tree(item)
    if (root / 'build/regen').exists():
        freed += remove_tree(root / 'build/regen')
    return freed


def select_game(argument, roots):
    if argument:
        game = Path(argument).expanduser().resolve()
        if not game.is_dir():
            raise BuildError(f'{game} is not a folder. Pass the folder Steam opens with Manage > Browse local files.')
        return game, manifest_for(game)
    installs = find_installs(roots)
    if not installs:
        raise BuildError('Hollow Knight was not found in Steam. Install it in Steam, or pass the game folder '
                         'with --game "<folder Steam opens with Manage > Browse local files>".')
    return installs[0]


def main(argv=None):
    parser = argparse.ArgumentParser(description='Build HKPSP game data from your Steam copy of Hollow Knight.')
    parser.add_argument('--game', help='Hollow Knight folder (found in Steam automatically when omitted)')
    parser.add_argument('--tools', default=str(ROOT / 'bundle.json'),
                        help='bundle.json listing the third-party tools (the release has one)')
    parser.add_argument('--check', action='store_true', help='Only run the checks')
    parser.add_argument('--keep', action='store_true',
                        help='Keep the work files (about 25 GB) so a rebuild takes minutes, not hours')
    args = parser.parse_args(argv)
    # The build writes inside the HKPSP folder (build/, _private/): unpack the release on a drive
    # with enough space. A separate work folder needs build-steam.ps1 support first.
    work = ROOT
    logs = work / 'build/psp/steam-build'
    summary = []
    try:
        game, manifest = select_game(args.game, steam_roots())
        build, label = check_game(game, manifest)
        summary += [f'Game: {game}', f'Version: {label}, build {build}']
        for warning in check_work_dir(work):
            summary.append('WARNING: ' + warning)
        tools = load_tools(Path(args.tools))
        print('\n'.join(summary))
        print('Checks passed.')
        if args.check:
            return 0
        env = bundle_env(Path(args.tools), tools)
        started = time.time()
        print('Building; this takes a few hours. You can stop and run again later: finished steps are kept.')
        code = run_build(build_command(tools, game), logs / 'hkpsp-build.log', env)
        if code != 0:
            raise BuildError(f'A build step failed (exit {code}).')
        package = ROOT / 'build/psp/package-game/PSP'
        # Only an unpacked release cleans up: a developer checkout keeps its own data.
        if not args.keep and 'prebuilt' in tools:
            print(f'Removed {clean_work() / 2**30:.1f} GB of work files (use --keep to keep them).')
        print(f'Done in {(time.time() - started) / 3600:.1f} h. Copy the folder {package} '
              'to the root of the PSP memory stick.')
        return 0
    except BuildError as error:
        print('ERROR: ' + str(error), file=sys.stderr)
        if logs.is_dir():
            target = support_zip(work / 'hkpsp-support.zip', sorted(logs.glob('*.log')),
                                 '\n'.join(summary + ['ERROR: ' + str(error)]) + '\n')
            print(f'For a bug report attach {target}.', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
