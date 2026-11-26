"""Export Sly's shop item icons from the game files.

Replaces the icons captured from a running game: each shop flag maps to the
original Sprite shown by its ShopItemStats, decoded with UnityPy at its own
rectangle. Writes `<flag>.png` and `<flag>.json` (`name`, `width`, `height`)
for `tools/convert_sly_shop.py`.
"""
import argparse
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/psp'))

# Shop flag -> Sprite name. Room_shop is level17; its item sprites are in
# sharedassets17, the shared inventory sprites in resources.assets.
ICONS = {
    'gotCharm_1': 'charm_sprite_02',
    'gotCharm_4': '_0006_charm_stalwart_shell',
    'hasLantern': 'Lumafly_Lantern0003',
    'slyRancidEgg': 'inv_rancid_egg',
    'slyShellFrag1': 'Heart_Piece_01',
    'slyShellFrag2': 'Heart_Piece_01',
    'slySimpleKey': 'inv_item__00014_graveyard_key',
    'slyVesselFrag1': 'Mini_Soul_Orb_Collect',
}
COLLECTIONS = ('resources.assets', 'sharedassets17.assets')


def sprite_image(sprite):
    """Crop to the whole-pixel sprite rectangle, as the running game shows it.

    UnityPy rounds fractional rectangles up, adding one row and column (the
    Simple Key is 133.93 x 124.95). Matching the captured icons, the extra top
    row and right column are dropped.
    """
    image = sprite.image.convert('RGBA')
    rect = sprite.m_RD.textureRect
    width, height = int(rect.width), int(rect.height)
    return image.crop((0, image.height - height, width, image.height))


def export(game_data, output):
    import UnityPy

    wanted = set(ICONS.values())
    sprites = {}
    for name in COLLECTIONS:
        for obj in UnityPy.load(str(game_data / name)).objects:
            if obj.type.name != 'Sprite':
                continue
            sprite = obj.read()
            if sprite.m_Name in wanted and sprite.m_Name not in sprites:
                sprites[sprite.m_Name] = sprite
    missing = wanted - sprites.keys()
    if missing:
        raise ValueError(f'Missing original shop icon sprites: {sorted(missing)}')
    output.mkdir(parents=True, exist_ok=True)
    for flag, name in ICONS.items():
        image = sprite_image(sprites[name])
        image.save(output / f'{flag}.png')
        (output / f'{flag}.json').write_text(json.dumps(dict(name=name, width=image.width, height=image.height)))
    print(f'Sly shop icons: {len(ICONS)} items from {len(sprites)} original sprites')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--game-data', type=Path, default=ROOT / '_private/original/Hollow Knight/hollow_knight_Data')
    parser.add_argument('--output', type=Path, default=ROOT / '_private/research/sly-shop-icons')
    args = parser.parse_args()
    export(args.game_data, args.output)


if __name__ == '__main__':
    main()
