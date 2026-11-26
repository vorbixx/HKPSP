"""Convert the authored Crossroads_47 Stag station Bell/Grate artwork.

The source package is an immutable AssetRipper export.  This converter keeps
the original tk2d clips and sprite geometry, and writes a small actor-room
record consumed by the StagStationToll runtime adapter.
"""
import argparse
import hashlib
import json
import math
import struct
import subprocess
from pathlib import Path

import converted_files  # Protect hardlinked converted output on direct invocation.
from PIL import Image

from assetripper_client import ROOT
from atlas_packing import pack_pages
from convert_hero import decode_sprite
from convert_world_switch_visuals import bank_bytes
from private_output_paths import is_private_output
from unity_transform import project
from tool_paths import TEX3DS as TEX3DS_EXE

CLIPS = ('Box_idle', 'Box_disappear', 'Bell_idle', 'Bell_stand_appear',
         'Bell_stand', 'Grate_idle', 'Grate_disappear', 'Front')
PAGE_SIZE = (1024, 1024)


def require(ok, message):
    if not ok:
        raise ValueError(message)


def source_data(art):
    manifest = json.loads((art / 'sources.json').read_text())
    require(manifest.get('scene') == 'Crossroads_47', 'Wrong Stag station source scene')
    for name, digest in manifest.get('sha256', {}).items():
        path = (art / name).resolve()
        require(path.is_relative_to(art.resolve()) and hashlib.sha256(path.read_bytes()).hexdigest() == digest,
                'Changed original Stag station export: ' + name)

    def part(name):
        entry = manifest['parts'][name]
        path = art / entry['file']
        return json.loads(path.read_text())['m_Structure']

    collection = part('206-collection')
    library = part('206-library')
    collection_id = int(Path(manifest['parts']['206-collection']['file']).stem.rsplit('-', 1)[1])
    definitions = collection['spriteDefinitions']
    selected = []
    for name in CLIPS:
        matches = [c for c in library['clips'] if c.get('name') == name]
        require(len(matches) == 1, 'Missing/duplicate source clip: ' + name)
        clip = matches[0]
        require(math.isfinite(clip['fps']) and clip['fps'] > 0 and clip['frames'],
                'Invalid source clip: ' + name)
        require(clip['wrapMode'] in (0, 1, 2, 3, 6), 'Unsupported source wrap mode: ' + name)
        require(0 <= clip.get('loopStart', 0) < len(clip['frames']), 'Invalid source loop start: ' + name)
        for frame in clip['frames']:
            require(frame['spriteCollection'] == dict(m_FileID=0, m_PathID=collection_id),
                    'External animation collection: ' + name)
            require(0 <= frame['spriteId'] < len(definitions) and not frame.get('triggerEvent'),
                    'Invalid source frame: ' + name)
        selected.append(clip)
    require(selected[1]['wrapMode'] == 2 and selected[6]['wrapMode'] == 2,
            'One-shot Box/Grate clips must retain source completion')
    ids = sorted({f['spriteId'] for c in selected for f in c['frames']})
    return manifest, collection, selected, ids


def room_bytes(art, scene_source, sprite_ids):
    """Write SSA1: four source draw records in actor slot order."""
    cache = {}

    def read(ident):
        if ident in cache:
            return cache[ident]
        for root in (art, scene_source):
            path = root / f'level77-{ident}.json'
            if path.is_file():
                cache[ident] = json.loads(path.read_text())
                return cache[ident]
        raise ValueError(f'Missing Station transform {ident}')

    def chain(transform):
        rows = []
        seen = set()
        while transform:
            ident = transform['m_GameObject']['m_PathID']
            if ident in seen:
                raise ValueError('Cyclic Station transform hierarchy')
            seen.add(ident)
            rows.append(transform)
            parent = transform['m_Father']['m_PathID']
            transform = read(parent) if parent else None
        return rows

    def draw(owner, transform_id, sprite_id, renderer_id):
        transform = read(transform_id)
        origin = project((0, 0, 0), chain(transform))
        px = project((1, 0, 0), chain(transform))
        py = project((0, 1, 0), chain(transform))
        sprite = read({'206': 1030, '209': 1028, '205': 1029, '17': 1021}[str(owner)])
        structure = sprite['m_Structure']
        scale = structure['_scale']
        colour = structure['_color']
        renderer = read(renderer_id)
        # tk2d uses the sprite scale as a local transform.  The source station
        # exports all four actors with unit scale; retain the explicit check so
        # a changed export cannot silently skew the room.
        require(scale == dict(x=1, y=1, z=1), f'Changed Station sprite scale {owner}')
        values = [px[0] - origin[0], py[0] - origin[0], origin[0],
                  px[1] - origin[1], py[1] - origin[1], origin[1], origin[2],
                  colour['r'], colour['g'], colour['b'], colour['a'],
                  renderer['m_SortingLayer'], renderer['m_SortingOrder'],
                  px[2] - origin[2], py[2] - origin[2], 0]
        require(all(math.isfinite(v) for v in values), f'Invalid Station draw {owner}')
        return struct.pack('<2I16f', sprite_ids.index(sprite_id), owner, *values)

    # Order matches the source painter order: box, bell, front, grate.
    records = [draw(206, 537, 20, 681), draw(209, 538, 13, 682),
               draw(205, 540, 33, 680), draw(17, 353, 58, 671)]
    return struct.pack('<4sI', b'SSA1', len(records)) + b''.join(records)


def convert(art, output, tex3ds, scene_source):
    art, output, scene_source = art.resolve(), output.absolute(), scene_source.resolve()
    require(is_private_output(output), 'Station output must remain private')
    manifest, collection, clips, sprite_ids = source_data(art)
    require(len(manifest.get('textures', [])) == 1, 'Station requires one source texture')
    texture = art / manifest['textures'][0]
    atlas = Image.open(texture).convert('RGBA')
    sprites = [(sid, *decode_sprite(atlas, collection['spriteDefinitions'][sid])) for sid in sprite_ids]
    # The source atlas contains the full Bell/Grate animation set.  Three
    # resident RGBA4 pages (6 MiB) are within the actor budget and avoid
    # dropping any authored frames.
    records, pages = pack_pages(sprites, [PAGE_SIZE, PAGE_SIZE, PAGE_SIZE])
    output.mkdir(parents=True, exist_ok=True)
    for i, page in enumerate(pages):
        png = output / f'stag-station-art-page-{i}.png'
        page.save(png)
        subprocess.run([str(tex3ds), '-f', 'rgba4', '-z', 'none', '-o',
                        str(output / f'stag-station-art-page-{i}.t3x'), str(png)], check=True)
    (output / 'stag-station-art.bin').write_bytes(bank_bytes(clips, sprite_ids, records, len(pages)))
    (output / 'stag-station-art-room.bin').write_bytes(room_bytes(art, scene_source, sprite_ids))
    report = dict(scene='Crossroads_47', format='HKA1/SSA1', texture_format='RGBA4',
                  page_size=PAGE_SIZE, pages=len(pages), sprites=sprite_ids,
                  clips=[c['name'] for c in clips], source_sha256=manifest['sha256'])
    (output / 'stag-station-art-manifest.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(sprites=len(sprite_ids), clips=len(clips), pages=len(pages))))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--art', type=Path, default=ROOT / '_private/minimum-obstacles/stag-station-art')
    parser.add_argument('--output', type=Path, default=ROOT / '_private/converted/stag-station-art')
    parser.add_argument('--scene-source', type=Path,
                        default=ROOT / '_private/research/crossroads-scenes/Crossroads_47/visuals')
    parser.add_argument('--tex3ds', type=Path, default=Path(TEX3DS_EXE))
    args = parser.parse_args()
    convert(args.art, args.output, args.tex3ds, args.scene_source)
