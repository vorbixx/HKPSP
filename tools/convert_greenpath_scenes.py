"""Build Greenpath/Cliffs packages through the generic pipeline at original scale."""
import converted_files  # Install copy-on-write before touching private outputs.
import argparse
import json
from pathlib import Path

from convert_scenes import (ROOT, install_terrain, metadata, export_scene_lighting, convert_art,
                            export_scene, write_scene_remasks, observe_scene_remasks)
from greenpath_scenes import SCENES


def convert(scene, romfs, terrain_stage=None, obstacle_sources=None):
    source = ROOT / '_private/research/crossroads-scenes' / scene
    output = ROOT / '_private/converted/scenes' / scene
    target = romfs / 'scenes' / scene
    output.mkdir(parents=True, exist_ok=True)
    target.mkdir(parents=True, exist_ok=True)
    lighting = export_scene_lighting(source, target)
    (output / 'lighting.json').write_text(json.dumps(lighting, indent=2) + '\n')
    install_terrain(source, output, target, terrain_stage)
    metadata(source, output, target, scene, obstacle_sources=obstacle_sources)
    convert_art(source, output, target, actor_romfs=romfs)
    export_scene(source / 'terrain', source / 'visuals', output, target)
    write_scene_remasks(observe_scene_remasks(source / 'visuals'), target / 'scene-remasks.bin')
    print(f'{scene}: texture scale 1', flush=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scene', choices=SCENES, required=True)
    parser.add_argument('--romfs', type=Path, default=ROOT / '_private/converted/hero/romfs')
    parser.add_argument('--terrain-stage', type=Path)
    parser.add_argument('--obstacle-sources', type=Path,
                        help='Recovered hazard-marker dependencies with sources.json provenance')
    args = parser.parse_args(argv)
    convert(args.scene, args.romfs, args.terrain_stage, args.obstacle_sources)


if __name__ == '__main__':
    main()
