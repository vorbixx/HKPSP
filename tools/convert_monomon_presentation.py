"""Original archive dialogue, anchors, and bounded MaskShatter frame stream."""
import argparse
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import xml.etree.ElementTree as ET
import converted_files
from assetripper_client import ROOT
from convert_cornifer import wrap_text
import text_fragment

KEYS = ('QUIRREL_TEACHER_1A', 'QUIRREL_TEACHER_1B', 'QUIRREL_TEACHER_1C',
        'QUIRREL_TEACHER_REPEAT', 'QUIRREL_TEACHER_NO_DN')


def extract(snapshot):
    sys.path.insert(0, str(ROOT/'_private/tools/boss-python'))
    import UnityPy
    from extract_tutorial_prompts import decrypt_language
    original = ROOT/'_private/original/Hollow Knight/hollow_knight_Data'
    env = UnityPy.load(str(original/'resources.assets'))
    matches = [o.read() for o in env.objects if o.type.name == 'TextAsset'
               and o.read().m_Name == 'EN_Quirrel']
    if len(matches) != 1:
        raise ValueError('Expected one original EN_Quirrel TextAsset')
    snapshot.mkdir(parents=True, exist_ok=True)
    (snapshot/'EN_Quirrel.xml').write_text(decrypt_language(matches[0].m_Script), encoding='utf-8')
    # CinematicVideoReference, level402: original 348-frame 30fps MaskShatter.
    with (original/'sharedassets400.resource').open('rb') as stream:
        stream.seek(1192928)
        movie = stream.read(2457449)
    (snapshot/'MaskShatter.mp4').write_bytes(movie)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--extract', action='store_true')
    args = parser.parse_args()
    snapshot = ROOT/'_private/minimum-bosses/monomon-presentation'
    output = ROOT/'_private/converted/monomon-presentation'
    if args.extract:
        extract(snapshot)
    output.mkdir(parents=True, exist_ok=True)
    entries = {e.attrib['name']: ''.join(e.itertext()) for e in ET.parse(snapshot/'EN_Quirrel.xml').iter('entry')}
    # Fail on unknown language keys instead of silently inventing dialogue.
    keys = KEYS[:3]
    lines = ['#pragma once', 'namespace hk3ds::monomon_presentation_data {']
    # Original dialogue goes to the text pack; the header keeps "@Quirrel/<key>".
    fragment = text_fragment.write(output, 'Quirrel', {key: wrap_text(entries[key]) for key in keys})
    for i, key in enumerate(keys):
        lines.append(f'inline constexpr char conversation{i}[] = {json.dumps(fragment[key])};')
    lines.extend(['}', ''])
    (ROOT/'include/monomon_presentation_data.hpp').write_text('\n'.join(lines), encoding='utf-8', newline='\n')
    # The bundled private copy is optional; a fresh checkout uses ffmpeg on PATH.
    bundled = ROOT/'_private/tools/ffmpeg-7.1.1'
    ffmpeg = next(bundled.rglob('ffmpeg.exe'), None) if bundled.is_dir() else None
    ffmpeg = ffmpeg or shutil.which('ffmpeg')
    if not ffmpeg:
        raise ValueError('Install ffmpeg or put ffmpeg.exe on PATH to decode MaskShatter')
    raw = output/'mask-shatter.rgb'
    # approximation: 200x112 pixels, bilinear enlargement to the 400px screen.
    # Preserve all original frames and their 30fps timing; no resident movie atlas.
    subprocess.run([str(ffmpeg), '-v', 'error', '-y', '-i', str(snapshot/'MaskShatter.mp4'),
                    '-an', '-vf', 'scale=200:112', '-pix_fmt', 'rgb565le', '-f', 'rawvideo', str(raw)], check=True)
    size = 200*112*2
    if raw.stat().st_size != size*348:
        raise ValueError('MaskShatter original frame count changed')
    with raw.open('rb') as source, (output/'mask-shatter.bin').open('wb') as target:
        target.write(struct.pack('<4s4I', b'HKM1', 200, 112, 348, 30))
        while frame := source.read(size):
            target.write(frame)
    raw.unlink()
    print('MaskShatter: 348 original frames; 15.59 MB SD; two 64 KiB GPU buffers; 1.344 MB/s RGB stream')


if __name__ == '__main__':
    main()
