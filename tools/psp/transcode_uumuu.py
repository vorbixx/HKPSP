"""Generate the PSP Archives gate and effect banks from the frozen source sheets."""
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from transcode_room import REPO, source_root, transcode_atlas


def main():
    source = source_root() / '_private/converted'
    output = REPO / '_private/converted-psp/anim-libraries'
    output.mkdir(parents=True, exist_ok=True)
    for name, sheets in (
        ('uumuu-gate', ('uumuu-gate-page-0.png',)),
        ('uumuu-effects', ('uumuu-effects-page-0.png', 'uumuu-blood-page.png')),
    ):
        directory = source / name
        table = f'{name}-art.bin'
        with tempfile.TemporaryDirectory(dir=output) as temp:
            package = Path(temp)
            shutil.copyfile(directory / 'romfs' / table, package / table)
            for index, sheet in enumerate(sheets):
                shutil.copyfile(directory / sheet, package / f'page-{index}.png')
            result = transcode_atlas(package, table, 'page-{n}.png', output / name / f'{name}-art', name)
        print(f"{name}: {result['pages']} PSP pages, {result['texture_bytes']} bytes")


if __name__ == '__main__':
    main()
