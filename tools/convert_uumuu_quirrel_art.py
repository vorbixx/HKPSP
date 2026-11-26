"""Extract or repack the original Uumuu/Quirrel presentation clips."""
import argparse
import importlib
import json
import math
import os
from pathlib import Path
import struct
import sys

import converted_files  # Install copy-on-write protection before writing private artifacts.
from assetripper_client import ROOT
from watcher_atlas import repack
from uumuu_art_layout import QUIRREL_PAGES, validate_runtime_layout
from tool_paths import TEX3DS as TEX3DS_EXE


STEM = 'uumuu-quirrel'
ALIASES = {
    'Quirrel': ('sharedassets229.assets', 82, {
        'Quirrel Arrive': 'Quirrel Arrive',
        'Quirrel Leave': 'Quirrel Leave',
        'Quirrel Watch': 'Quirrel Watch',
        'Slash Antic': 'Slash Antic',
        'Slash': 'Slash',
    }),
}
SOURCE_CLIPS = tuple(ALIASES['Quirrel'][2].values())


def _cached_clips(source):
    actors = [actor for actor in source['actors'] if actor.get('name') == 'Quirrel Land']
    if len(actors) != 1:
        raise ValueError('Expected one Quirrel Land actor in cached source metadata')
    animators = [component for component in actors[0]['components']
                 if component.get('type') == 'tk2dSpriteAnimator']
    if len(animators) != 1:
        raise ValueError('Expected one Quirrel Land animator in cached source metadata')
    clips = animators[0].get('animation_library', {}).get('clips', [])
    result = {}
    for name in SOURCE_CLIPS:
        matches = [clip for clip in clips if clip.get('name') == name]
        if len(matches) != 1:
            raise ValueError(f'Expected one cached {name} clip')
        result[name] = matches[0]
    return result


def _signature(clip):
    return (float(clip['fps']), int(clip['wrapMode']), int(clip['loopStart']),
            [(int(frame['spriteCollection']['m_FileID']),
              int(frame['spriteCollection']['m_PathID']), int(frame['spriteId']))
             for frame in clip['frames']])


def _validate_source_sprite_parts(manifest, bank_path, original_count, tile_count):
    encoded = manifest.get('source_sprite_parts')
    if encoded is None:
        if tile_count != original_count:
            raise ValueError('Unmapped extra source sprite tiles in HKA1 bank')
        return
    if int(manifest.get('source_sprite_count', -1)) != original_count or len(encoded) != original_count:
        raise ValueError('Source sprite part mapping has the wrong original sprite count')
    pairs = []
    for index in range(original_count):
        value = encoded.get(str(index))
        if not isinstance(value, list) or len(value) != 2:
            raise ValueError(f'Missing source sprite part mapping for index {index}')
        first, tail = value
        pairs.append((int(first), 0xffffffff if tail is None else int(tail)))
    sidecar = Path(bank_path).with_name(f'{STEM}-parts.bin')
    data = sidecar.read_bytes()
    if len(data) != 12 + original_count * 8:
        raise ValueError('HQP1 sidecar has an unexpected length')
    magic, sidecar_originals, sidecar_tiles = struct.unpack_from('<4sII', data)
    actual = [struct.unpack_from('<II', data, 12 + index * 8) for index in range(original_count)]
    if (magic != b'HQP1' or sidecar_originals != original_count or sidecar_tiles != tile_count or
            actual != pairs):
        raise ValueError('HQP1 sidecar does not match the source sprite mapping')
    claimed = []
    for index, (first, tail) in enumerate(actual):
        if first != index or first >= tile_count:
            raise ValueError(f'Invalid first tile for source sprite {index}')
        claimed.append(first)
        if tail != 0xffffffff:
            if tail < original_count or tail >= tile_count:
                raise ValueError(f'Invalid tail tile for source sprite {index}')
            claimed.append(tail)
    if sorted(claimed) != list(range(tile_count)):
        raise ValueError('HQP1 sidecar does not map every tile exactly once')


def validate_snapshot(source, manifest, bank_path):
    """Check selected cached clip data against the extracted HKA1 bank."""
    if source.get('build_id') != 22529139 or source.get('scene') != 'level229':
        raise ValueError('Expected cached build 22529139 level229 metadata')
    cached = _cached_clips(source)
    library_clips = manifest.get('source_clips', {}).get('Quirrel', [])
    by_name = {clip.get('name'): clip for clip in library_clips}
    sprite_ids = set()
    collection_refs = set()
    for name in SOURCE_CLIPS:
        if name not in by_name or _signature(cached[name]) != _signature(by_name[name]):
            raise ValueError(f'Extracted source clip differs from cached {name}')
        for file_id, path_id, sprite_id in _signature(cached[name])[3]:
            collection_refs.add((file_id, path_id))
            sprite_ids.add(sprite_id)
    if len(collection_refs) != 1:
        raise ValueError(f'Expected one shared source sprite collection, got {sorted(collection_refs)}')

    data = Path(bank_path).read_bytes()
    magic, pages, sprites, clip_count, frame_count = struct.unpack_from('<4s4I', data)
    if magic != b'HKA1' or clip_count != len(SOURCE_CLIPS):
        raise ValueError('Extracted bank has an unexpected HKA1 header')
    clip_offset = 20 + sprites * 36
    clip_offset_bytes = struct.calcsize('<48sfIIII')
    frame_offset = clip_offset + clip_count * clip_offset_bytes
    sorted_ids = sorted(sprite_ids)
    _validate_source_sprite_parts(manifest, bank_path, len(sorted_ids), sprites)
    consumed_frames = 0
    for index, name in enumerate(SOURCE_CLIPS):
        offset = clip_offset + index * clip_offset_bytes
        raw_name, fps, wrap, loop_start, start, count = struct.unpack_from('<48sfIIII', data, offset)
        clip = cached[name]
        alias = next(alias for alias, original in ALIASES['Quirrel'][2].items() if original == name)
        if (raw_name.split(b'\0', 1)[0].decode() != f'Quirrel/{alias}' or
                not math.isclose(fps, float(clip['fps']), rel_tol=0, abs_tol=1e-5) or
                wrap != int(clip['wrapMode']) or loop_start != int(clip['loopStart']) or
                count != len(clip['frames']) or start != consumed_frames):
            raise ValueError(f'Extracted HKA1 clip differs from cached {name}')
        actual = struct.unpack_from(f'<{count}I', data, frame_offset + start * 4) if count else ()
        expected = tuple(sorted_ids.index(frame['spriteId']) for frame in clip['frames'])
        if actual != expected:
            raise ValueError(f'Extracted HKA1 frame order differs from cached {name}')
        consumed_frames += count
    if consumed_frames != frame_count:
        raise ValueError('Extracted HKA1 frame count does not match its clip table')
    return dict(clips=clip_count, frame_slots=frame_count, unique_sprites=len(sorted_ids),
                tile_sprites=sprites, source_pages=pages)


def _check_external_mapping(source_dir, packages):
    """Verify the copied level229 fileID 3 mapping before selecting its source bank."""
    # Compare unresolved paths: -UseInstalledFiles links _private/original to
    # the read-only Steam folder, and the source must still be that input.
    source_dir = Path(os.path.abspath(source_dir))
    private_original = Path(os.path.abspath(ROOT / '_private/original'))
    if not source_dir.is_relative_to(private_original):
        raise ValueError('Extraction source must be under _private/original (copied original only)')
    sys.path.insert(0, str(packages))
    from watcher_unity import AlignedGenerator  # noqa: F401
    unitypy = importlib.import_module('UnityPy')
    env = unitypy.Environment(path=str(source_dir))
    scene_path = source_dir / 'level229'
    env.load_file(str(scene_path))
    scene = next((file for file in env.files.values() if getattr(file, 'name', None) == 'level229'), None)
    if scene is None or len(scene.externals) < 3:
        raise ValueError('Could not resolve copied level229 external fileID 3')
    mapped = Path(scene.externals[2].path).name
    if mapped != 'sharedassets229.assets':
        raise ValueError(f'level229 fileID 3 maps to {mapped}, not sharedassets229.assets')
    return mapped


def extract(args, cached):
    _check_external_mapping(args.source, args.packages)
    type_tree = importlib.import_module('UnityPy.helpers.TypeTreeGenerator')
    from watcher_unity import AlignedGenerator
    type_tree.TypeTreeGenerator = AlignedGenerator
    from convert_crossroads_bosses import convert

    args.scale = 1
    args.snapshot = None
    args.save_snapshot = args.snapshot_dir
    args.output = ROOT / '_private/converted/uumuu-quirrel-source'
    args.packages = str(args.packages)
    convert(args, aliases=ALIASES, stem=STEM, component_scenes=[], split_oversized=True,
            complete_library=False)
    manifest_path = args.snapshot_dir / 'manifest.json'
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    manifest['limitations'] = [
        'Original Quirrel Land and Slash clips only; FSM sequencing and audio remain separate.',
        'Source scale 1; original sprite pixels and world-space geometry are retained.',
    ]
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    result = validate_snapshot(cached, manifest, args.snapshot_dir / f'{STEM}-art.bin')
    print(json.dumps(dict(extracted=True, **result), sort_keys=True))


def repack_snapshot(args, cached):
    manifest = json.loads((args.snapshot_dir / 'manifest.json').read_text(encoding='utf-8'))
    validate_snapshot(cached, manifest, args.snapshot_dir / f'{STEM}-art.bin')
    repack(args.snapshot_dir, args.output, args.tex3ds, stem=STEM)
    result = validate_snapshot(cached, json.loads((args.output / 'manifest.json').read_text(encoding='utf-8')),
                               args.output / 'romfs' / f'{STEM}-art.bin')
    packed = json.loads((args.output / 'manifest.json').read_text(encoding='utf-8'))
    validate_runtime_layout(packed, QUIRREL_PAGES)
    print(json.dumps(dict(extracted=False, **result, page_dimensions=packed['page_dimensions'],
                          texture_bytes=packed['texture_bytes']), sort_keys=True))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--extract', action='store_true', help='extract from the copied original; run under -Exclusive')
    parser.add_argument('--source', type=Path, default=ROOT / '_private/original/Hollow Knight/hollow_knight_Data')
    parser.add_argument('--snapshot', dest='snapshot_dir', type=Path,
                        default=ROOT / '_private/minimum-bosses/quirrel-art')
    parser.add_argument('--output', type=Path, default=ROOT / '_private/converted/uumuu-quirrel')
    parser.add_argument('--packages', type=Path, default=ROOT / '_private/tools/boss-python')
    parser.add_argument('--tex3ds', type=Path, default=Path(TEX3DS_EXE))
    args = parser.parse_args()
    source_file = args.snapshot_dir.parent / 'uumuu' / 'level229-source.json'
    cached = json.loads(source_file.read_text(encoding='utf-8'))
    if args.extract:
        extract(args, cached)
    else:
        repack_snapshot(args, cached)


if __name__ == '__main__':
    main()
