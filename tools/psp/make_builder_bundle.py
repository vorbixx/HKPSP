"""Assemble the third-party part of the player builder bundle (docs/tasks/psp-user-builder.md).

Reads the pinned tool list of one platform: tools/psp/builder-bundle.json (Windows) or
builder-bundle-linux.json (Linux, assemble it on Linux so file modes and links survive). Local
entries are copied from the developer machine; download entries must already be unpacked in
--downloads/<name>; 'system' entries are found on the player's machine at build time. Writes
<output>/bundle.json (tool name -> path, read by hkpsp_build.py) and <output>/THIRD-PARTY.txt
(version, license, source of every tool).

--release also puts the HKPSP sources of HEAD (git archive), the start file (`Build HKPSP.bat`
or `build-hkpsp.sh`) and prebuilt/ (game EBOOT and replay host built from HEAD, with
prebuilt.json hashes) into the output, so the player needs no compiler, make or WSL. It refuses a
tree with uncommitted psp/, src/, include/. The EBOOT is the same on both systems: Linux takes it
with --eboot from the Windows release (it is built in WSL from Windows).

  python tools/psp/make_builder_bundle.py --check
  python tools/psp/make_builder_bundle.py --output build/psp/builder-bundle --downloads <folder>
  python tools/psp/make_builder_bundle.py --release --output <empty folder> --downloads <folder>
  python3 tools/psp/make_builder_bundle.py --platform linux --release --eboot <Windows bundle>/prebuilt/EBOOT.PBP \\
      --pwsh <linux pwsh> --output <empty folder> --downloads ~/hkpsp-dl
"""
import argparse
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MANIFESTS = {'windows': Path(__file__).with_name('builder-bundle.json'),
             'linux': Path(__file__).with_name('builder-bundle-linux.json')}
MANIFEST = MANIFESTS['windows']
GIT = 'C:/Program Files/Git/cmd/git.exe' if os.name == 'nt' else 'git'
# Start file and prebuilt replay host per platform; build-steam.ps1 -Prebuilt checks the hashes.
START = {'windows': 'tools/psp/bundle/Build HKPSP.bat', 'linux': 'tools/psp/bundle/build-hkpsp.sh'}
HOST = {'windows': ('hkpsp-host.exe', 'build/psp/host/hkpsp-host.exe'),
        'linux': ('hkpsp-host', 'build/psp/host-linux/hkpsp-host')}
PREBUILT = {'EBOOT.PBP': 'psp/EBOOT.PBP', HOST['windows'][0]: HOST['windows'][1]}
CODE_PATHS = ('psp', 'src', 'include', 'tools/host.mk')


def load_manifest(platform):
    manifest = json.loads(MANIFESTS[platform].read_text(encoding='utf-8'))
    if 'python_packages_from' in manifest:
        other = json.loads(MANIFESTS[platform].with_name(manifest['python_packages_from']).read_text(encoding='utf-8'))
        manifest['python_packages'] = other['python_packages']
    return manifest


def source_root():
    """Developer tool folder: HKPSP_SOURCE_ROOT/_private/tools, as build-config.ps1 resolves it."""
    base = Path(os.environ['HKPSP_SOURCE_ROOT']) if os.environ.get('HKPSP_SOURCE_ROOT') else ROOT.parent / 'HK3DS'
    return base / '_private/tools'


def tool_source(tool, tools_dir, downloads):
    """Folder the tool is copied from, 'system' for a library of the player's machine, or None."""
    if tool.get('system'):
        return 'system'
    if tool.get('download'):
        folder = downloads / tool['name'] if downloads else None
    else:
        local = Path(tool['local'])
        folder = local if local.is_absolute() else tools_dir / local
    return folder if folder and (folder / tool['exe']).is_file() else None


def plan(manifest, tools_dir, downloads):
    """[(tool, source folder, 'system' or None)] for every pinned tool."""
    return [(tool, tool_source(tool, tools_dir, downloads)) for tool in manifest['tools']]


def copy_tool(tool, source, output):
    target = output / tool['bundle']
    target.mkdir(parents=True, exist_ok=True)
    if 'files' in tool:
        for name in tool['files']:
            shutil.copy2(source / name, target / name)
    else:
        shutil.copytree(source, target, symlinks=True, dirs_exist_ok=True)
    return target / tool['exe']


def third_party_text(manifest):
    lines = ['HKPSP builder: third-party tools shipped in this folder.', '']
    for tool in manifest['tools']:
        lines.append(f"{tool['name']}: {tool['version']}; license {tool['license']}; source {tool['url']}")
    lines += ['', 'Python packages (installed into runtime/python): ' + ', '.join(manifest['python_packages'])]
    return '\n'.join(lines) + '\n'


def assemble(manifest, tools_dir, downloads, output):
    entries = plan(manifest, tools_dir, downloads)
    missing = [tool['name'] for tool, source in entries if source is None]
    if missing:
        raise SystemExit('Not available: ' + ', '.join(missing) +
                         '. Unpack the pinned downloads into --downloads/<name> (see the manifest).')
    bundle = {}
    for tool, source in entries:
        if source == 'system':
            bundle[tool['name']] = 'system:' + tool['system']
        else:
            bundle[tool['name']] = copy_tool(tool, source, output).relative_to(output).as_posix()
    (output / 'bundle.json').write_text(json.dumps(bundle, indent=2) + '\n', encoding='utf-8')
    (output / 'THIRD-PARTY.txt').write_text(third_party_text(manifest), encoding='utf-8')
    return bundle


def git(*args, root=ROOT):
    return subprocess.run([GIT, '-C', str(root), *args], check=True, capture_output=True).stdout


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as handle:
        for block in iter(lambda: handle.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest().upper()


def write_prebuilt(sources, output, commit):
    """Copy the prebuilt files into output/prebuilt and record their hashes."""
    target = output / 'prebuilt'
    target.mkdir(parents=True, exist_ok=True)
    files = {}
    for name, source in sources.items():
        shutil.copy2(source, target / name)
        files[name] = sha256(target / name)
    info = {'commit': commit, 'files': files}
    (target / 'prebuilt.json').write_text(json.dumps(info, indent=2) + '\n', encoding='utf-8')
    return info


def add_release(output, platform='windows', root=ROOT, eboot=None, pwsh='pwsh'):
    """HKPSP sources of HEAD, the start file and the prebuilt programs."""
    if git('status', '--porcelain', '--', *CODE_PATHS, root=root).strip():
        raise SystemExit('Commit or stash the changes under psp/, src/, include/ first: '
                         'the prebuilt programs must match the shipped sources.')
    commit = git('rev-parse', 'HEAD', root=root).decode().strip()
    with tarfile.open(fileobj=io.BytesIO(git('archive', '--format=tar', 'HEAD', root=root))) as archive:
        archive.extractall(output, filter='data')
    # Build (or confirm up to date) from the clean tree: make.ps1 is stamp-based, host-build
    # incremental. The PSP EBOOT is built in WSL from Windows only.
    scripts = ['tools/psp/host-build.ps1'] + ([] if eboot else ['tools/psp/make.ps1'])
    if platform != 'windows' and not eboot:
        raise SystemExit('Pass --eboot <Windows release>/prebuilt/EBOOT.PBP: the PSP build runs from Windows.')
    for script in scripts:
        subprocess.run([pwsh, '-NoProfile', '-File', str(root / script)], check=True, cwd=root)
    start = root / START[platform]
    shutil.copy2(start, output / start.name)
    if platform != 'windows':
        (output / start.name).chmod(0o755)
    host_name, host_path = HOST[platform]
    return write_prebuilt({'EBOOT.PBP': Path(eboot) if eboot else root / 'psp/EBOOT.PBP',
                           host_name: root / host_path}, output, commit)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--check', action='store_true', help='List what is available; copy nothing')
    parser.add_argument('--platform', choices=sorted(MANIFESTS), default='windows' if os.name == 'nt' else 'linux')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/psp/builder-bundle')
    parser.add_argument('--downloads', type=Path, help='Folder with the unpacked pinned downloads')
    parser.add_argument('--release', action='store_true',
                        help='Also add the HEAD sources, the start file and the prebuilt programs')
    parser.add_argument('--eboot', type=Path, help='Prebuilt EBOOT.PBP to ship (Linux: from the Windows release)')
    parser.add_argument('--pwsh', default='pwsh', help='PowerShell 7 used to build the replay host')
    args = parser.parse_args(argv)
    manifest = load_manifest(args.platform)
    if args.check:
        for tool, source in plan(manifest, source_root(), args.downloads):
            state = (f'ready: {source}' if source else
                     ('needs download' if tool.get('download') else 'MISSING locally'))
            print(f"{tool['name']:15} {tool['version'][:45]:45} {state}")
        return 0
    if args.release and args.output.exists() and any(args.output.iterdir()):
        raise SystemExit(f'{args.output} is not empty; a release bundle starts from an empty folder.')
    bundle = assemble(manifest, source_root(), args.downloads, args.output)
    if args.release:
        info = add_release(args.output, args.platform, eboot=args.eboot, pwsh=args.pwsh)
        bundle['prebuilt'] = 'prebuilt'
        (args.output / 'bundle.json').write_text(json.dumps(bundle, indent=2) + '\n', encoding='utf-8')
        print(f"Release bundle from commit {info['commit']}")
    print(f'Bundle tools in {args.output}: ' + ', '.join(bundle))
    return 0


if __name__ == '__main__':
    sys.exit(main())
