"""Export the original language sheets and the Russian fonts.

- Every English sheet (`EN_*` TextAsset in resources.assets), decrypted, to
  `_private/research/english-text`: tools/psp/pack_text.py builds the game's
  text pack from them, so the source and the EBOOT carry no original text.
- The Russian sheets the port shows and the two original RU outline fonts,
  the second source of the font fallback (tools/psp/transcode_fonts.py), to
  `_private/research/russian-text` and `_private/research/russian-fonts`.
"""
import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))

# The RU sheets of every English sheet the port shows (resources.assets).
RU_SHEETS = ('Titles', 'Dream Witch', 'Dreamers', 'Enemy Dreams', 'Minor NPC', 'Cornifer', 'UI', 'Prices',
             'Lore Tablets', 'Credits List', 'Stag', 'Prompts', 'Sly', 'Quirrel')
# Original Font assets: resources.assets:1406 and sharedassets0.assets:73.
FONTS = {'Perpetua_ru_edited': ('resources.assets', 'Perpetua_ru_edited.ttf'),
         'TrajanPro-Bold_ru': ('sharedassets0.assets', 'TrajanPro-Bold_ru.otf')}


def sheet_file(language, sheet):
    return f'{language}_{sheet.replace(" ", "_")}-decrypted.xml'


def font_bytes(font):
    data = font.get('m_FontData')
    if not data:
        raise ValueError(f'{font.get("m_Name")}: no embedded font data')
    return bytes(data)


def collect(game_data):
    import UnityPy

    english, russian, fonts = {}, {}, {}
    wanted = {f'RU_{sheet}' for sheet in RU_SHEETS}
    for collection in sorted({'resources.assets', *(c for c, _ in FONTS.values())}):
        for obj in UnityPy.load(str(game_data / collection)).objects:
            if obj.type.name not in ('TextAsset', 'Font'):
                continue
            tree = obj.read_typetree()
            name = tree.get('m_Name')
            if obj.type.name == 'TextAsset' and collection == 'resources.assets' and (
                    name in wanted or name.startswith('EN_')):
                table = english if name.startswith('EN_') else russian
                if name in table:
                    raise ValueError(f'Duplicate original sheet {name}')
                table[name] = tree['m_Script']
            elif obj.type.name == 'Font' and name in FONTS and FONTS[name][0] == collection:
                if name in fonts:
                    raise ValueError(f'Duplicate original font {name}')
                fonts[name] = font_bytes(tree)
    missing = sorted((wanted - russian.keys()) | (FONTS.keys() - fonts.keys()))
    if missing or not english:
        raise ValueError(f'Missing original language sources: {missing or "English sheets"}')
    return english, russian, fonts


def export(game_data, research, decrypt=None):
    if decrypt is None:
        from extract_tutorial_prompts import decrypt_language as decrypt
    english, russian, fonts = collect(game_data)
    targets = {'english-text': english, 'russian-text': russian}
    for folder, sheets in targets.items():
        directory = research / folder
        directory.mkdir(parents=True, exist_ok=True)
        for name, encoded in sorted(sheets.items()):
            xml = decrypt(encoded)
            if '<entries' not in xml:
                raise ValueError(f'{name}: decrypted sheet has no entries')
            language, sheet = name.split('_', 1)
            # Keep the original LF line endings.
            (directory / sheet_file(language, sheet)).write_text(xml, encoding='utf-8', newline='')
    font_dir = research / 'russian-fonts'
    font_dir.mkdir(parents=True, exist_ok=True)
    for name, (_, filename) in FONTS.items():
        (font_dir / filename).write_bytes(fonts[name])
    print(f'Language sources: {len(english)} English sheets, {len(russian)} Russian sheets, {len(FONTS)} fonts')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--game-data', type=Path, default=ROOT / '_private/original/Hollow Knight/hollow_knight_Data')
    parser.add_argument('--research', type=Path, default=ROOT / '_private/research')
    args = parser.parse_args()
    export(args.game_data, args.research)


if __name__ == '__main__':
    main()
