"""Build the game's text pack from the original English sheets.

Reads `text_spec.json` (keys the code uses) and the converter fragments
(`text-fragment.json` beside each converter's output: keys and text of generated tables),
rebuilds every string from `_private/research/english-text`, and writes
`_private/converted-psp/text/text.bin` with `format.txt` (the data format
from psp/include/data_format.hpp). Layout: see src/text_pack.cpp.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import text_spec

SHEETS = ROOT / '_private/research/english-text'
# Converters that write `text-fragment.json` beside their output (tools/text_fragment.py).
FRAGMENTS = tuple(ROOT / '_private/converted' / name / 'text-fragment.json'
                  for name in ('cornifer-original', 'stag', 'sly-shop', 'monomon-presentation'))
OUTPUT = ROOT / '_private/converted-psp/text'
FORMAT_HEADER = ROOT / 'psp/include/data_format.hpp'


def english_sheets(directory=SHEETS):
    sheets = {}
    for path in sorted(directory.glob('EN_*-decrypted.xml')):
        sheets[path.name[3:-len('-decrypted.xml')]] = text_spec.sheet_entries(path)
    if not sheets:
        raise ValueError(f'No English sheets in {directory}; run tools/psp/export_language_sources.py')
    return sheets


def fragments(paths=FRAGMENTS):
    texts = {}
    for path in paths:
        if not path.is_file():
            raise ValueError(f'Missing converter text {path}; run tools/regen.ps1')
        for key, text in json.loads(path.read_text(encoding='utf-8')).items():
            if key in texts and texts[key] != text:
                raise ValueError(f'{path.name}: conflicting text for {key}')
            texts[key] = text
    return texts


def data_format(header=FORMAT_HEADER):
    match = re.search(r'dataFormat=(\d+);', header.read_text(encoding='utf-8'))
    if not match:
        raise ValueError(f'{header}: no dataFormat')
    return int(match[1])


def keys_hash(keys):
    """Hash of the sorted key list: data_format.hpp records it for the current code."""
    return hashlib.sha256('\n'.join(sorted(keys)).encode('utf-8')).hexdigest()[:16]


def encode(texts):
    keys = sorted(texts, key=lambda key: key.encode('utf-8'))
    blob, offsets = bytearray(), []
    base = 12 + 8 * len(keys)
    for key in keys:
        if '\0' in key or '\0' in texts[key]:
            raise ValueError(f'{key}: NUL in text')
        key_at = base + len(blob)
        blob += key.encode('utf-8') + b'\0'
        text_at = base + len(blob)
        blob += texts[key].encode('utf-8') + b'\0'
        offsets.append((key_at, text_at))
    return b'HKTX' + struct.pack('<II', 1, len(keys)) + b''.join(struct.pack('<II', *o) for o in offsets) + bytes(blob)


def build(spec_path=text_spec.SPEC, sheets_dir=SHEETS, fragment_paths=FRAGMENTS):
    texts = text_spec.build(text_spec.load_spec(spec_path), english_sheets(sheets_dir))
    for key, text in fragments(fragment_paths).items():
        if key in texts and texts[key] != text:
            raise ValueError(f'{key}: spec and converter fragment disagree')
        texts[key] = text
    return texts


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--output', type=Path, default=OUTPUT)
    args = parser.parse_args()
    texts = build()
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / 'text.bin').write_bytes(encode(texts))
    (args.output / 'format.txt').write_text(f'{data_format()}\n', encoding='ascii', newline='\n')
    print(f'Text pack: {len(texts)} keys, data format {data_format()}, keys {keys_hash(texts)}')


if __name__ == '__main__':
    main()
