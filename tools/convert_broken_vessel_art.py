"""Pack original Broken Vessel and balloon clips without resampling."""
import converted_files
import argparse
import importlib
import json
import os
import sys
import struct
from pathlib import Path
from assetripper_client import ROOT
from tool_paths import TEX3DS as TEX3DS_EXE


def append_projectile(args):
    """Append the prefab's original SpriteRenderer as a single-frame HKA1 clip."""
    import UnityPy
    from PIL import Image
    env = UnityPy.Environment(path=str(args.source))
    env.load_file(str(args.source / 'sharedassets343.assets'))
    assets = next(f for f in env.files.values() if getattr(f, 'name', '') == 'sharedassets343.assets')
    sprite = assets.objects[23].read()  # Projectile DS/SH -> head_glob SpriteRenderer.
    image = sprite.image.convert('RGBA')
    rect, pivot, offset = sprite.m_Rect, sprite.m_Pivot, sprite.m_RD.textureRectOffset
    ppu = sprite.m_PixelsToUnits
    left = (offset.x - pivot.x * rect.width) / ppu
    bottom = (offset.y - pivot.y * rect.height) / ppu
    geometry = (left * .5078403353691101, bottom * .507840096950531,
                (left + image.width / ppu) * .5078403353691101,
                (bottom + image.height / ppu) * .507840096950531)
    snapshot = args.save_snapshot
    path = snapshot / 'broken-vessel-art.bin'
    data = path.read_bytes()
    magic, pages, sprites, clips, frames = struct.unpack_from('<4s4I', data)
    if magic != b'HKA1':
        raise ValueError('Expected original animation bank')
    sprite_end, clip_end = 20 + sprites * 36, 20 + sprites * 36 + clips * 68
    record = struct.pack('<5I4f', pages, 1, 1, image.width, image.height, *geometry)
    clip = struct.pack('<48sf4I', b'Projectile/Idle', 1, 6, 0, frames, 1)
    path.write_bytes(struct.pack('<4s4I', magic, pages + 1, sprites + 1, clips + 1, frames + 1)
                     + data[20:sprite_end] + record + data[sprite_end:clip_end] + clip
                     + data[clip_end:] + struct.pack('<I', sprites))
    page = Image.new('RGBA', (1024, 1024))
    page.paste(image, (1, 1))
    page.save(snapshot / f'broken-vessel-page-{pages}.png')
    manifest_path = snapshot / 'manifest.json'
    manifest = json.loads(manifest_path.read_text())
    manifest.update(pages=pages + 1, sprites=sprites + 1)
    manifest['projectile_source'] = 'sharedassets343.assets Sprite23, prefab head_glob'
    manifest_path.write_text(json.dumps(manifest, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT/'_private/original/Hollow Knight/hollow_knight_Data')
    parser.add_argument('--extract', action='store_true', help='Explicitly decode copied original assets instead of using the saved snapshot')
    parser.add_argument('--snapshot', type=Path, default=ROOT/'_private/minimum-bosses/broken-vessel/art')
    parser.add_argument('--output', type=Path, default=ROOT/'_private/converted/broken-vessel')
    parser.add_argument('--packages', type=Path, default=Path(os.environ.get('HK3DS_BOSS_PACKAGES', ROOT/'_private/tools/boss-python')))
    parser.add_argument('--tex3ds', type=Path, default=Path(TEX3DS_EXE))
    args = parser.parse_args()
    if not args.extract:
        from watcher_atlas import repack
        repack(args.snapshot, args.output, args.tex3ds, stem='broken-vessel')
        return
    sys.path.insert(0, str(args.packages))
    from watcher_unity import AlignedGenerator
    importlib.import_module('UnityPy.helpers.TypeTreeGenerator').TypeTreeGenerator = AlignedGenerator
    data = json.loads((ROOT/'_private/minimum-bosses/broken-vessel/level343-source.json').read_text())
    boss = next(a for a in data['actors'] if a['name'] == 'Infected Knight')
    animator = next(c for c in boss['components'] if c['type'] == 'tk2dSpriteAnimator')
    clips = animator['animation_library']['clips']
    aliases = {'BrokenVessel': ('sharedassets343.assets', 150,
               {c['name']: c['name'] for c in clips if c['name'] and c['frames']}),
               'Balloon': ('sharedassets343.assets', 148,
               {name: name for name in ('Spawn','Chase','Death','TurnToChase','Leave')}),
               'Scuttler': ('sharedassets53.assets',39,
               {name:name for name in ('Spawn','Land','Run')}),
               'ScuttlerSplash': ('resources.assets',21298,{'Splat':'Enemy Shot'})}
    args.scale = 1
    # Save the snapshot where the broken-vessel-art rule reads it (--snapshot).
    args.save_snapshot = args.snapshot
    args.snapshot = None
    from convert_crossroads_bosses import convert
    convert(args, aliases=aliases, stem='broken-vessel', component_scenes=[],
            sprite_tints={'ScuttlerSplash': (1, .556862772, .20784314)}, complete_library=False)
    append_projectile(args)
    from watcher_atlas import repack
    repack(args.save_snapshot, args.output, args.tex3ds, stem='broken-vessel')


if __name__ == '__main__':
    main()
