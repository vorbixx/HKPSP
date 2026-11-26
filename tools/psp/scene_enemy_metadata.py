"""Regenerate CPU-only scene enemy banks with original animation names.

HK3DS converted banks can predate the removal of synthetic Idle aliases.
Use the current converter and read-only research; never rewrite those inputs.
The generated texture files are intermediate and are not staged for PSP.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import transcode_room as room
room.import_pil()
import convert_crossroads_enemies_a as converter
from tool_paths import TEX3DS as TEX3DS_EXE


def convert(source, assets):
    output = assets / 'scene-enemies'
    if not output.resolve().is_relative_to((room.REPO / '_private/converted-psp').resolve()):
        raise ValueError('Scene metadata output must remain in converted-psp')
    output.mkdir(parents=True, exist_ok=True)
    intermediate = output / 'art'
    intermediate.mkdir(exist_ok=True)
    root = source / '_private/converted/hero/romfs'
    reports = {}
    for stem in converter.STEMS:
        folder = source / '_private/research' / ('buzzer' if stem == 'vengefly' else 'crossroads-enemies-a')
        mapping = json.loads((folder / ('buzzer-map.json' if stem == 'vengefly' else stem + '-map.json')).read_bytes())
        for name, digest in mapping['source_sha256'].items():
            if hashlib.sha256((folder / name).read_bytes()).hexdigest() != digest:
                raise ValueError('Changed enemy source ' + name)
        shutil.copyfile(root / (stem + '.bin'), intermediate / (stem + '.bin'))
        reports[stem] = dict(source_sha256=mapping['source_sha256'], artwork=converter.artwork(
            folder, mapping, converter.STEMS.index(stem), intermediate,
            output, Path(TEX3DS_EXE)))
    for name in ('enemy-b.bin', 'enemy-b-anim.bin', 'enemy-c-anim.bin'):
        # Enemy C: this checkout's own bank (its battle snapshot adds every original sentry,
        # Flying Sentry and Vomit Glob clip); the shared source root keeps the older one.
        own = room.REPO / '_private/converted/enemy-c' / name
        shutil.copyfile(own if own.is_file() else root / name, output / name)
    (output / 'manifest.json').write_text(json.dumps(reports, indent=2) + '\n')
    print('Scene enemy metadata: 11 CPU banks regenerated/copied')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=room.source_root())
    parser.add_argument('--assets', type=Path, default=room.REPO / '_private/converted-psp')
    args = parser.parse_args()
    convert(args.source, args.assets)
