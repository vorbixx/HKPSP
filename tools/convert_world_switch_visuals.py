"""Build a private HKA1 bank from Crossroads_03's original switch/gate clips.

This prepares local sprite geometry; room transforms, save hooks and renderer
admission remain separate. No source image is resized or replaced.
"""
import argparse
import hashlib
import json
import math
import struct
import subprocess
from pathlib import Path

from PIL import Image

import converted_files  # Protect hardlinked converted output on direct invocation.

from assetripper_client import ROOT
from atlas_packing import pack_pages
from convert_hero import decode_sprite
from world_switch_room import room_data, room_bytes
from tool_paths import TEX3DS as TEX3DS_EXE

CLIPS = ('Switch Idle', 'Switch Hit', 'Toll Gate Idle', 'Toll Gate Open')
# RoomRenderer admits 1024-square RGBA4 pages; keep its 2 MiB allocation.
PAGE_SIZE = (1024, 1024)


def write_texture(sprites, output, tex3ds):
    records, pages = pack_pages(sprites, [PAGE_SIZE])
    png = output / 'world-switch-page-0.png'
    pages[0].save(png)
    subprocess.run([str(tex3ds), '-f', 'rgba4', '-z', 'none', '-o',
                    str(output / 'world-switch-page-0.t3x'), str(png)], check=True)
    return records


def source_clips(library, collection_id, definition_count):
    """Reject unsupported references/events instead of silently losing behavior."""
    result = []
    for name in CLIPS:
        matches = [clip for clip in library['clips'] if clip['name'] == name]
        if len(matches) != 1:
            raise ValueError('Missing/duplicate source clip: ' + name)
        clip = matches[0]
        if not math.isfinite(clip['fps']) or clip['fps'] <= 0 or not clip['frames']:
            raise ValueError('Invalid source timing: ' + name)
        if clip['wrapMode'] not in (0, 1, 2, 3, 6):
            raise ValueError('Unsupported source wrap mode: ' + name)
        if not 0 <= clip['loopStart'] < len(clip['frames']):
            raise ValueError('Invalid source loop start: ' + name)
        for frame in clip['frames']:
            ref = frame['spriteCollection']
            if ref != dict(m_FileID=0, m_PathID=collection_id):
                raise ValueError('External animation collection: ' + name)
            if not 0 <= frame['spriteId'] < definition_count:
                raise ValueError('Invalid source sprite: ' + name)
            if frame.get('triggerEvent'):
                raise ValueError('Source animation event requires runtime support: ' + name)
        result.append(clip)
    if result[1]['wrapMode'] != 2 or result[3]['wrapMode'] != 2:
        raise ValueError('Hit/open clips must retain source Once completion')
    return result


def bank_bytes(clips, sprite_ids, records, page_count=1):
    if not isinstance(page_count, int) or not 0 < page_count <= 32:
        raise ValueError('Invalid animation page count')
    remap = {sprite_id: index for index, sprite_id in enumerate(sprite_ids)}
    data = bytearray(struct.pack('<4s4I', b'HKA1', page_count, len(sprite_ids), len(clips),
                                 sum(len(clip['frames']) for clip in clips)))
    for sprite_id in sprite_ids:
        data.extend(struct.pack('<5I4f', *records[sprite_id]))
    offset = 0
    for clip in clips:
        data.extend(struct.pack('<48sf4I', clip['name'].encode('ascii'), clip['fps'],
                                clip['wrapMode'], clip['loopStart'], offset, len(clip['frames'])))
        offset += len(clip['frames'])
    for clip in clips:
        for frame in clip['frames']:
            data.extend(struct.pack('<I', remap[frame['spriteId']]))
    return data


def convert(source, output, tex3ds, scene_source=None):
    source, output = source.resolve(), output.absolute()
    if not output.resolve().is_relative_to((ROOT / '_private/converted').resolve()):
        raise ValueError('Converted original resources must stay in own _private/converted')
    manifest = json.loads((source / 'sources.json').read_text())
    if manifest['scene'] != 'Crossroads_03':
        raise ValueError('Wrong source scene')
    for filename, expected in manifest['sha256'].items():
        path = (source / filename).resolve()
        if not path.is_relative_to(source) or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise ValueError('Changed source export: ' + filename)
    def part(kind):
        filename = manifest['parts'][kind]['file']
        if filename not in manifest['sha256']:
            raise ValueError('Missing source provenance: ' + filename)
        return json.loads((source / filename).read_text())['m_Structure']
    collection, library = part('collection'), part('library')
    if len(collection['textures']) != 1 or len(manifest['textures']) != 1:
        raise ValueError('Only one verified original texture is supported')
    if manifest['textures'][0] not in manifest['sha256']:
        raise ValueError('Texture provenance missing')
    collection_id = int(Path(manifest['parts']['collection']['file']).stem.rsplit('-', 1)[1])
    clips = source_clips(library, collection_id, len(collection['spriteDefinitions']))
    sprite_ids = sorted({frame['spriteId'] for clip in clips for frame in clip['frames']})
    scene_source = scene_source or ROOT / '_private/research/crossroads-scenes/Crossroads_03/visuals'
    scene_hashes = {}
    def read_scene(filename):
        raw = (scene_source / filename).read_bytes()
        scene_hashes[filename] = hashlib.sha256(raw).hexdigest()
        return json.loads(raw)
    room = room_data(read_scene, library, sprite_ids)
    atlas = Image.open(source / manifest['textures'][0]).convert('RGBA')
    sprites = [(sid, *decode_sprite(atlas, collection['spriteDefinitions'][sid])) for sid in sprite_ids]
    output.mkdir(parents=True, exist_ok=True)
    records = write_texture(sprites, output, tex3ds)
    (output / 'world-switch.bin').write_bytes(bank_bytes(clips, sprite_ids, records))
    (output / 'world-switch-room.bin').write_bytes(room_bytes(room))
    report = dict(scene=manifest['scene'], source_sha256=manifest['sha256'], format='HKA1',
                  page_size=PAGE_SIZE, texture_format='RGBA4',
                  texture_bytes=PAGE_SIZE[0]*PAGE_SIZE[1]*2, raster_scale=1,
                  sprites=[dict(source_id=sid, name=collection['spriteDefinitions'][sid]['name'],
                                record=records[sid]) for sid in sprite_ids],
                  clips=clips, room=room, scene_source_sha256=scene_hashes,
                  admitted_to_romfs=output.resolve().is_relative_to((ROOT / '_private/converted/hero/romfs').resolve()))
    (output / 'world-switch-manifest.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(dict(sprites=len(sprite_ids), clips=len(clips), page_size=PAGE_SIZE,
                          texture_bytes=report['texture_bytes'],
                          admitted_to_romfs=report['admitted_to_romfs'])))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / '_private/minimum-obstacles/switch-art')
    parser.add_argument('--output', type=Path, default=ROOT / '_private/converted/world-switch-visuals')
    parser.add_argument('--tex3ds', type=Path, default=Path(TEX3DS_EXE))
    parser.add_argument('--scene-source', type=Path,
                        default=ROOT / '_private/research/crossroads-scenes/Crossroads_03/visuals')
    args = parser.parse_args()
    convert(args.source, args.output, args.tex3ds, args.scene_source)
