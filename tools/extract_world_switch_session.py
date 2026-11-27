"""Recover only Crossroads_03 switch art in an isolated AssetRipper session.

Run through validation-lock.ps1 -Exclusive. Inputs are the immutable private
original copy. No source image is synthesized and the server is always stopped.
"""
import argparse
import os
import re
from pathlib import Path
import shutil
import subprocess
import time
import urllib.parse
import urllib.request

from assetripper_client import Client, ROOT
from extract_world_switch import extract


def run(port, output, *, scene='Crossroads_03', level=39, extractor=extract, input_root=ROOT):
    original = input_root / '_private/original/Hollow Knight/hollow_knight_Data'
    output = output.absolute()
    if not output.is_relative_to((ROOT / '_private').resolve()):
        raise ValueError('Extraction output must remain in this worktree private area')
    label = 'switch' if scene == 'Crossroads_03' else scene.lower()
    inputs = output.parent / f'{label}-input/hollow_knight_Data'
    inputs.mkdir(parents=True, exist_ok=True)
    names = {f'level{level}', 'resources.assets', 'resources.assets.resS',
                 'globalgamemanagers', 'globalgamemanagers.assets',
                 'globalgamemanagers.assets.resS', 'sharedassets0.assets',
                 'sharedassets0.assets.resS', 'sharedassets0.resource',
                 f'sharedassets{level}.assets', f'sharedassets{level}.assets.resS', f'sharedassets{level}.resource'}
    # Same bounded dependency discovery as capture_original_frames.py: scene
    # externals contain the serialized sharedasset names. Do not load all rooms.
    for raw in set(re.findall(rb'sharedassets[0-9]+\.assets', (original / f'level{level}').read_bytes())):
        name = raw.decode('ascii')
        names.update((name, name + '.resS', name.removesuffix('.assets') + '.resource'))
    print('Source inputs:', ', '.join(sorted(names)), flush=True)
    for name in sorted(names):
        source, target = original / name, inputs / name
        if source.is_file() and not target.exists():
            try:
                os.link(source, target)
            except OSError:
                # Linking needs write access to the game file; a read-only or protected game
                # folder (another volume, locked-down Steam library) denies it. Copy instead.
                shutil.copy2(source, target)
    if not (inputs / 'Managed').exists():
        shutil.copytree(original / 'Managed', inputs / 'Managed')
    log_path = ROOT / f'build/obstacle-{label}-assetripper.log'
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open('wb') as log:
        process = subprocess.Popen(
            # The Steam build driver passes the user's AssetRipper; development
            # checkouts keep the bundled private copy.
            [os.environ.get('HKPSP_ASSETRIPPER') or
             str(input_root / '_private/tools/AssetRipper-2.0.0/AssetRipper.GUI.Free.exe'),
             '--headless', '--port', str(port)], stdout=log, stderr=subprocess.STDOUT,
            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        try:
            client = Client(port)
            deadline = time.monotonic() + 60
            while True:
                if process.poll() is not None:
                    raise RuntimeError(f'AssetRipper exited; see {log_path}')
                try:
                    client.request('/')
                    break
                except OSError:
                    if time.monotonic() >= deadline:
                        raise TimeoutError(f'AssetRipper startup; see {log_path}')
                    time.sleep(1)
            request = urllib.request.Request(
                client.base + '/LoadFolder',
                data=urllib.parse.urlencode({'path': str(inputs)}).encode())
            with urllib.request.urlopen(request, timeout=1200) as response:
                response.read()
            extractor(client, ROOT / f'_private/research/crossroads-scenes/{scene}/visuals', output)
            print('Original interaction art:', output)
        finally:
            process.terminate()
            try:
                process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            # The staged copy is only for this session. On another drive the
            # originals cannot be hardlinked, so each copy costs ~1.5 GB.
            if os.environ.get('HKPSP_ASSETRIPPER'):
                shutil.rmtree(inputs.parent, ignore_errors=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=58326)
    parser.add_argument('--output', type=Path, default=ROOT / '_private/minimum-obstacles/switch-art')
    args = parser.parse_args()
    run(args.port, args.output)
