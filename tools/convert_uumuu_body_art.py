"""Extract or repack the original Uumuu body presentation clips."""
import argparse
import importlib
import json
import math
from pathlib import Path
import struct

import converted_files  # Install copy-on-write protection before private writes.
from assetripper_client import ROOT
from convert_uumuu_quirrel_art import _check_external_mapping, _signature
from watcher_atlas import repack
from uumuu_art_layout import BODY_PAGES, validate_runtime_layout
from tool_paths import TEX3DS as TEX3DS_EXE


STEM = 'uumuu-body'
ALIASES = {
    'Uumuu': ('sharedassets229.assets', 82, {
        'Idle': 'Idle',
        'Attack': 'Attack',
        'Wound': 'Wound',
        'Recover Antic': 'Recover Antic',
        'Recover': 'Recover',
    }),
}
SOURCE_CLIPS = tuple(ALIASES['Uumuu'][2].values())
EXPECTED_CLIPS = {
    'Idle': (12.0, 0, 0, 6),
    'Attack': (12.0, 1, 2, 5),
    'Wound': (12.0, 1, 2, 9),
    'Recover Antic': (15.0, 0, 0, 3),
    'Recover': (12.0, 1, 1, 7),
}


def _cached_clips(source):
    actors = [actor for actor in source['actors'] if actor.get('name') == 'Mega Jellyfish']
    if len(actors) != 1:
        raise ValueError('Expected one Mega Jellyfish actor in cached source metadata')
    animators = [component for component in actors[0]['components']
                 if component.get('type') == 'tk2dSpriteAnimator']
    if len(animators) != 1:
        raise ValueError('Expected one Mega Jellyfish animator in cached source metadata')
    animator = animators[0]
    if animator.get('data', {}).get('library') != {'m_FileID': 3, 'm_PathID': 82}:
        raise ValueError('Mega Jellyfish animator no longer references fileID 3 pathID 82')
    clips = animator.get('animation_library', {}).get('clips', [])
    result = {}
    for name in SOURCE_CLIPS:
        matches = [clip for clip in clips if clip.get('name') == name]
        if len(matches) != 1:
            raise ValueError(f'Expected one cached {name} clip')
        clip = matches[0]
        fps, wrap, loop_start, frame_count = EXPECTED_CLIPS[name]
        if (float(clip['fps']), int(clip['wrapMode']), int(clip['loopStart']), len(clip['frames'])) != (
                fps, wrap, loop_start, frame_count):
            raise ValueError(f'Cached {name} metadata differs from the source contract')
        result[name] = clip
    return result


def validate_snapshot(source, manifest, bank_path):
    """Validate cached Uumuu clips and exact frame order in an unsplit HKA1 bank."""
    if source.get('build_id') != 22529139 or source.get('scene') != 'level229':
        raise ValueError('Expected cached build 22529139 level229 metadata')
    cached = _cached_clips(source)
    library_clips = manifest.get('source_clips', {}).get('Uumuu', [])
    by_name = {clip.get('name'): clip for clip in library_clips}
    sprite_ids, collection_refs = set(), set()
    for name in SOURCE_CLIPS:
        if name not in by_name or _signature(cached[name]) != _signature(by_name[name]):
            raise ValueError(f'Extracted source clip differs from cached {name}')
        for file_id, path_id, sprite_id in _signature(cached[name])[3]:
            collection_refs.add((file_id, path_id))
            sprite_ids.add(sprite_id)
    if len(collection_refs) != 1:
        raise ValueError(f'Expected one shared source sprite collection, got {sorted(collection_refs)}')
    if manifest.get('source_sprite_parts') is not None or manifest.get('source_sprite_count') is not None:
        raise ValueError('Uumuu body art must not use a split-sprite sidecar')
    if Path(bank_path).with_name(f'{STEM}-parts.bin').exists():
        raise ValueError('Unexpected split-sprite sidecar for Uumuu body art')

    data = Path(bank_path).read_bytes()
    if len(data) < 20:
        raise ValueError('HKA1 bank is truncated before its header')
    magic, pages, sprites, clip_count, frame_count = struct.unpack_from('<4s4I', data)
    if magic != b'HKA1' or clip_count != len(SOURCE_CLIPS):
        raise ValueError('Extracted bank has an unexpected HKA1 header')
    if pages != int(manifest.get('pages', -1)) or sprites != int(manifest.get('sprites', -1)):
        raise ValueError('HKA1 page or sprite count differs from its manifest')
    # Extraction uses the generic packer on the PC. Only repacked pages are
    # uploaded by the port. Exact allocation/admission uses the packed layout.
    if not math.isclose(float(manifest.get('bitmap_scale', 0)), 1.0, rel_tol=0, abs_tol=1e-8):
        raise ValueError('Uumuu body bank must preserve original scale 1 pixels')
    if sprites != len(sprite_ids):
        raise ValueError('HKA1 sprite count differs from the cached clips')
    sprite_offset = 20
    clip_offset = sprite_offset + sprites * 36
    clip_record_size = struct.calcsize('<48sfIIII')
    frame_offset = clip_offset + clip_count * clip_record_size
    if len(data) != frame_offset + frame_count * 4:
        raise ValueError('HKA1 bank length does not match its records')
    dimensions = manifest.get('page_dimensions', [(1024, 1024)] * pages)
    if len(dimensions) != pages:
        raise ValueError('Uumuu body page dimensions differ from its page count')
    for index in range(sprites):
        page, x, y, width, height = struct.unpack_from('<5I', data, sprite_offset + index * 36)
        if page >= pages or not width or not height or width > 1022 or height > 1022:
            raise ValueError(f'Uumuu body sprite {index} exceeds an unsplit 1024 atlas')
        page_width, page_height = dimensions[page]
        if x + width > page_width or y + height > page_height:
            raise ValueError(f'Uumuu body sprite {index} exceeds its page bounds')

    sorted_ids = sorted(sprite_ids)
    consumed_frames = 0
    for index, name in enumerate(SOURCE_CLIPS):
        offset = clip_offset + index * clip_record_size
        raw_name, fps, wrap, loop_start, start, count = struct.unpack_from('<48sfIIII', data, offset)
        clip = cached[name]
        alias = next(alias for alias, original in ALIASES['Uumuu'][2].items() if original == name)
        if (raw_name.split(b'\0', 1)[0].decode() != f'Uumuu/{alias}' or
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


def extract(args, cached):
    _check_external_mapping(args.source, args.packages)
    type_tree = importlib.import_module('UnityPy.helpers.TypeTreeGenerator')
    from watcher_unity import AlignedGenerator
    type_tree.TypeTreeGenerator = AlignedGenerator
    from convert_crossroads_bosses import convert

    args.scale = 1
    args.snapshot = None
    args.save_snapshot = args.snapshot_dir
    args.output = ROOT / '_private/converted/uumuu-body-source'
    args.packages = str(args.packages)
    convert(args, aliases=ALIASES, stem=STEM, component_scenes=[], split_oversized=False,
            complete_library=False)
    manifest_path = args.snapshot_dir / 'manifest.json'
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    manifest['limitations'] = [
        'Original Mega Jellyfish body clips only; FSM sequencing and completion effects remain separate.',
        'Source scale 1; original sprite pixels and world-space geometry are retained.',
        'Oversized sprites are rejected; this bank has no split-sprite runtime sidecar.',
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
    validate_runtime_layout(packed, BODY_PAGES)
    print(json.dumps(dict(extracted=False, **result, page_dimensions=packed['page_dimensions'],
                          texture_bytes=packed['texture_bytes']), sort_keys=True))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--extract', action='store_true', help='extract copied-original assets; run under -Exclusive')
    parser.add_argument('--source', type=Path, default=ROOT / '_private/original/Hollow Knight/hollow_knight_Data')
    parser.add_argument('--snapshot', dest='snapshot_dir', type=Path,
                        default=ROOT / '_private/minimum-bosses/uumuu-body-art')
    parser.add_argument('--output', type=Path, default=ROOT / '_private/converted/uumuu-body')
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
