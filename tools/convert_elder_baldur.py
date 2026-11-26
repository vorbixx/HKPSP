"""Convert only the copied Crossroads_11_alt Elder Baldur animation library."""
import argparse
import json
import os
from pathlib import Path
from assetripper_client import ROOT
from convert_crossroads_bosses import convert
from PIL import Image
import tex3ds_cache
import struct
from tool_paths import TEX3DS as TEX3DS_EXE


def encode_atlas(output, tex3ds, manifest):
    if manifest['pages'] != 1:
        raise ValueError('Elder Baldur exceeds its 2 MiB resident texture allowance')
    bank = (output / 'romfs/elder-baldur-art.bin').read_bytes()
    _, _, count, _, _ = struct.unpack_from('<4sIIII', bank)
    records = [struct.unpack_from('<5I4f', bank, 20 + index * 36) for index in range(count)]
    # Remove only unused atlas space, retaining a transparent sampling gutter.
    width = max(8, 1 << max(record[1] + record[3] for record in records).bit_length())
    height = max(8, 1 << max(record[2] + record[4] for record in records).bit_length())
    texture_bytes = width * height * 4
    if texture_bytes > 2 * 1024 * 1024:
        raise ValueError('Elder Baldur exceeds its 2 MiB resident texture allowance')
    png = output / 'elder-baldur-page-0.png'
    with Image.open(png) as image:
        if width > image.width or height > image.height:
            raise ValueError('Elder Baldur sprites exceed their source atlas')
        cropped = image.convert('RGBA').crop((0, 0, width, height))
    cropped.save(png)
    # Full color retains the shell's dark gradient without a four-bit hue bias.
    tex3ds_cache.run([str(tex3ds), '-f', 'rgba8', '-z', 'lz11', '-o',
                     str(output / 'romfs/elder-baldur-page-0.t3x'), str(png)],
                    check=True, capture_output=True)
    manifest.update(texture_format='rgba8', texture_bytes=texture_bytes,
                    texture_page_size=[width, height])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT/'_private/original/Hollow Knight/hollow_knight_Data')
    parser.add_argument('--output', type=Path, default=ROOT/'_private/converted/elder-baldur')
    parser.add_argument('--packages', type=Path, default=Path(os.environ.get('HK3DS_UNITYPY_PATH',str(ROOT/'_private/tools/boss-python'))))
    parser.add_argument('--tex3ds', type=Path, default=Path(TEX3DS_EXE))
    args = parser.parse_args()
    args.snapshot = args.save_snapshot = None
    # Keep more of the thin shell highlights while staying within the existing page allowance.
    args.scale = .375
    clips = ('Closed', 'Open', 'Idle', 'Close1', 'Close2', 'Hit', 'Shoot Antic', 'Shoot CD')
    # level50-4286 library fileID 5 / pathID 109 resolves to sharedassets50.
    aliases={'': ('sharedassets50.assets', 109, {c:c for c in clips}),
             'Goop': ('sharedassets32.assets',745,{c:c for c in ('Idle','Impact')}),
             'Roller': ('sharedassets50.assets',111,{c:c for c in ('Start','Stop','Idle','Roll','Death Air')})}
    convert(args, aliases=aliases,
            stem='elder-baldur', component_scenes=[('level50', {'Blocker'})], prune_stale=False)
    # The reusable converter prefixes clips; this actor has its own bank.
    path = args.output/'romfs/elder-baldur-art.bin'
    data = bytearray(path.read_bytes())
    _, _, sprites, count, _ = struct.unpack_from('<4sIIII', data)
    offset = 20 + sprites*36
    for i in range(count):
        start = offset + i*68
        name = bytes(data[start:start+48]).split(b'\0',1)[0].decode().removeprefix('/')
        data[start:start+48] = name.encode().ljust(48,b'\0')
    path.write_bytes(data)
    manifest_path = args.output/'manifest.json'
    manifest = json.loads(manifest_path.read_text())
    manifest['limitations'] = ['Sound, particles and full corpse effects are separate from the actor bank.']
    encode_atlas(args.output, args.tex3ds, manifest)
    manifest_path.write_text(json.dumps(manifest,indent=2),encoding='utf-8')


if __name__ == '__main__':
    main()
