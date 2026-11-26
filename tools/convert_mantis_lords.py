"""Convert the copied original level180 Mantis Lords animation library."""
import converted_files
import argparse
import importlib
import os
import sys
from pathlib import Path
from assetripper_client import ROOT
from tool_paths import TEX3DS as TEX3DS_EXE


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / '_private/original/Hollow Knight/hollow_knight_Data')
    parser.add_argument('--packages', type=Path,
                        default=Path(os.environ.get('HK3DS_BOSS_PACKAGES', ROOT / '_private/tools/boss-python')))
    parser.add_argument('--output', type=Path, default=ROOT / '_private/converted/mantis-lords')
    parser.add_argument('--tex3ds', type=Path, default=Path(TEX3DS_EXE))
    parser.add_argument('--inspect', action='store_true')
    parser.add_argument('--reference-projectile', type=Path,
                        help='Patch a resident bank using the retained original shot capture (approximate art)')
    args = parser.parse_args()
    if args.reference_projectile:
        from mantis_projectile_art import patch_cached_bank
        patch_cached_bank(args.output, args.reference_projectile, args.tex3ds)
        return
    print('Mantis: loading source types', flush=True)
    sys.path.insert(0, str(args.packages))
    from watcher_unity import AlignedGenerator
    importlib.import_module('UnityPy.helpers.TypeTreeGenerator').TypeTreeGenerator = AlignedGenerator
    import UnityPy
    # MonoScript dependencies live beside the copied assets. The default cwd
    # makes UnityPy recursively inventory this worktree and its private junctions.
    environment = UnityPy.Environment(path=str(args.source))
    generator = AlignedGenerator('6000.0.61f1')
    generator.load_local_dll_folder(str(args.source / 'Managed'))
    print('Mantis: loading encounter library', flush=True)
    environment.typetree_generator = generator
    environment.load_file(str(args.source / 'level180'))
    scene = next(f for f in environment.files.values() if getattr(f, 'name', '') == 'level180')
    library_name = Path(scene.externals[1].path).name
    environment.load_file(str(args.source / library_name))
    library_file = next(f for f in environment.files.values() if getattr(f, 'name', '') == library_name)
    clips = library_file.objects[84].read_typetree()['clips']
    aliases = {c['name']: c['name'] for c in clips if c['name'] and c['frames']}
    print('Mantis source library:', library_name, 84, flush=True)
    print('Mantis source clips:', ', '.join(aliases), flush=True)
    if args.inspect:
        environment.load_file(str(args.source / 'level179'))
        parent = next(f for f in environment.files.values() if getattr(f, 'name', '') == 'level179')
        transforms = {i: o.read_typetree() for i, o in parent.objects.items() if o.type.name == 'Transform'}
        by_owner = {t['m_GameObject']['m_PathID']: (i, t) for i, t in transforms.items()}
        def world(ident):
            transform = transforms[ident]
            point = transform['m_LocalPosition']
            father = transform['m_Father']['m_PathID']
            if not father:
                return point['x'], point['y']
            x, y = world(father)
            return x + point['x'], y + point['y']
        for ident, obj in parent.objects.items():
            if obj.type.name != 'GameObject':
                continue
            name = obj.read_typetree()['m_Name']
            if 'throne' in name.lower():
                transform_id, transform = by_owner[ident]
                print('Mantis throne:', ident, name, world(transform_id), transform['m_LocalScale'])
        return
    args.snapshot = args.save_snapshot = None
    args.scale = 1
    used = {'Dash Antic', 'Dash Arrive', 'Dash Attack', 'Dash Leave', 'Dash Recover', 'Death New 1', 'Death New 2',
            'Dstab Arrive', 'Dstab Attack', 'Dstab Land', 'Dstab Leave', 'Throw',
            'Throw Antic', 'Wall Arrive', 'Wall Leave 1', 'Wall Leave 2', 'Mantis Air Projectile',
            'Throne Idle', 'Throne Bow', 'Throne Wounded', 'Throne Look Up', 'Throne Look Down',
            'Throne Stand', 'Throne Leave'}
    aliases = {name: name for name in aliases if name in used}
    from convert_crossroads_bosses import convert
    from mantis_intro_art import title_sprite
    # level179 GO975 uses sharedassets168 library49/default clip9 (Gate Idle).
    convert(args, aliases={'Lord': (library_name, 84, aliases),
                           'Hero': ('resources.assets', 20600, {'Challenge Start': 'Challenge Start', 'Challenge End': 'Challenge End'}),
                           'Gate': ('sharedassets168.assets', 49, {'Closed': 'Gate Idle'}),
                           'Floor': ('sharedassets168.assets', 49, {'Idle': 'Floor Idle', 'Retract': 'Floor Retract', 'Extend': 'Floor Extend'})},
            # Death New 1 needs the full final page; retain source pixels/geometry.
            # Current residency admission includes this additional 1.5 MiB.
            stem='mantis', component_scenes=[], page_limit=12,
            page_sizes=[(1024, 1024)] * 12, static_clips=[title_sprite()],
            complete_library=False)


if __name__ == '__main__':
    main()
