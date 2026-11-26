"""Capture the title background from a private copy of the supported Windows game."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import sys
import tempfile
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from capture_ui_audit import capture_ui
from convert_title_menu import import_menu_background


def audit_source(original, manifest):
    match = re.search(r'"buildid"\s+"(\d+)"', manifest.read_text(encoding='utf-8-sig'))
    if match is None or match[1] != '22529139':
        raise ValueError('The title capture requires Steam build 22529139')
    inputs = ('hollow_knight.exe', 'UnityPlayer.dll', 'hollow_knight_Data/Managed/Assembly-CSharp.dll')
    files = []
    for name in inputs:
        with (original / name).open('rb') as stream:
            files.append(dict(file=name, sha256=hashlib.file_digest(stream, 'sha256').hexdigest()))
    return dict(build=match[1], method='Private original-player capture', files=files, validated=True)


def prepare(original, manifest, dotnet, cecil, title):
    if title.resolve() != title or not title.is_relative_to(ROOT / '_private'):
        raise ValueError(f'Title output must stay in the checkout private tree: {title}')
    provenance = title / 'provenance.json'
    background = title / 'menu-background-source.png'
    if not provenance.is_file():
        raise ValueError('Run the title source export before capturing its background')
    for entry in json.loads(provenance.read_text(encoding='utf-8'))['assets']:
        if entry['file'] == background.name and entry.get('capture_audit_sha256') and background.is_file():
            with background.open('rb') as stream:
                background_hash = hashlib.file_digest(stream, 'sha256').hexdigest()
            if background_hash == entry['sha256']:
                print('Title capture already prepared')
                return
    audit = audit_source(original, manifest)
    converted = ROOT / '_private/converted'
    if converted.resolve() != converted:
        raise ValueError(f'Capture workspace must stay inside the checkout: {converted}')
    converted.mkdir(parents=True, exist_ok=True)
    logs = ROOT / 'build/psp/steam-build'
    logs.mkdir(parents=True, exist_ok=True)
    args = SimpleNamespace(width=1920, height=1080, ui_audit='menus', dotnet=dotnet)
    with tempfile.TemporaryDirectory(prefix='title-capture-', dir=converted) as temporary:
        player = Path(temporary) / 'player'
        try:
            results = capture_ui(args, player_root=player, source_audit=audit,
                                 converted_inputs=False, cecil=cecil)
            import_menu_background(title, results / 'menu-background.png')
        finally:
            for name in ('player.log', 'stdout.log', 'results/provenance.json', 'results/error.txt'):
                if (player / name).is_file():
                    shutil.copyfile(player / name, logs / ('title-' + Path(name).name))
    print(f'Prepared title background at {background}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--steam-manifest', type=Path, required=True)
    parser.add_argument('--dotnet', type=Path, required=True)
    parser.add_argument('--cecil', type=Path, required=True)
    args = parser.parse_args()
    prepare(ROOT / '_private/original/Hollow Knight', args.steam_manifest, args.dotnet,
            args.cecil, ROOT / '_private/research/title-menu')
