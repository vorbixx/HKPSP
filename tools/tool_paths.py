"""External tool paths shared by the converters.

tex3ds: HKPSP_TEX3DS when set; otherwise the developer's devkitPro install; otherwise the copy
a release bundle ships in _private/tools/tex3ds (docs/tasks/psp-user-builder.md). The developer
path stays the default, so regen cache keys of existing checkouts do not change.
"""
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEVKITPRO_TEX3DS = 'C:/devkitPro/tools/bin/tex3ds.exe'
BUNDLED_TEX3DS = ROOT / '_private/tools/tex3ds/tex3ds.exe'


def find_tex3ds(env=os.environ, devkitpro=DEVKITPRO_TEX3DS, bundled=BUNDLED_TEX3DS):
    if env.get('HKPSP_TEX3DS'):
        return env['HKPSP_TEX3DS']
    if Path(devkitpro).is_file() or not Path(bundled).is_file():
        return devkitpro
    return Path(bundled).as_posix()


TEX3DS = find_tex3ds()
