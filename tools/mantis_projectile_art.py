"""Recover a temporary Mantis shot silhouette from a retained original capture.

This is a capture-derived fallback, not the missing tk2d sprite export. Normal
conversion uses the original Mantis Air Projectile animation instead.
"""
import hashlib
import json
import os
import shutil
import struct
import subprocess
from collections import deque

import numpy as np
from PIL import Image, ImageFilter

from assetripper_client import ROOT


REFERENCE_SHA256 = '16eb1f204b7c5e1e0e3aba0d9a134d716e2bf23acb3e477e0c477181fd2d5d7d'
CLIP = 'Lord/Mantis Air Projectile'


def append_floor_extension(clips, frame_ids, manifest):
    """Restore the native Extend sequence using only resident floor sprites."""
    by_name = {c[0].split(b'\0')[0].decode(): c for c in clips}
    if 'Floor/Extend' in by_name:
        return
    source = {c['name']: c for c in manifest['source_clips']['Floor']}
    def key(frame):
        collection = frame['spriteCollection']
        return collection['m_FileID'], collection['m_PathID'], frame['spriteId']
    resident = {}
    for alias, name in (('Idle', 'Floor Idle'), ('Retract', 'Floor Retract')):
        clip = by_name[f'Floor/{alias}']
        frames = source[name]['frames']
        if clip[-1] != len(frames):
            raise ValueError('Resident floor animation differs from source metadata')
        for offset, frame in enumerate(frames):
            resident[key(frame)] = frame_ids[clip[-2] + offset]
    extend = source['Floor Extend']
    try:
        mapped = [resident[key(frame)] for frame in extend['frames']]
    except KeyError as error:
        raise ValueError('Floor Extend requires a sprite absent from the resident bank') from error
    clips.append((b'Floor/Extend', extend['fps'], extend['wrapMode'],
                  extend['loopStart'], len(frame_ids), len(mapped)))
    frame_ids.extend(mapped)
    manifest['aliases']['Extend'] = 'Floor Extend'


def recover_publication(output):
    """Recover a stopped two-rename publication before reading any source pages."""
    staging = output.with_name(output.name + '.projectile-staging')
    backup = output.with_name(output.name + '.projectile-backup')
    for path in (staging, backup):
        if path.resolve().parent != output.resolve().parent or path.is_symlink():
            raise ValueError('Mantis publication path escaped its output directory')
    if backup.exists():
        if not output.exists():
            os.replace(backup, output)
        else:
            shutil.rmtree(backup)
    if staging.exists():
        shutil.rmtree(staging)
    return staging, backup


def publish(output, staging, backup):
    """Keep the previous complete bank until the staged bank has been installed."""
    os.replace(output, backup)
    try:
        os.replace(staging, output)
    except OSError:
        os.replace(backup, output)
        raise
    shutil.rmtree(backup)


def free_slot(pages, records, size):
    """Append a padded sprite without moving or recompressing other actor pages."""
    width, height = size[0] + 2, size[1] + 2
    for page, image in enumerate(pages):
        if width > image.width or height > image.height:
            continue
        occupied = np.zeros((image.height, image.width), dtype=np.int64)
        for source_page, x, y, w, h, *_ in records:
            if source_page == page:
                occupied[max(0, y-1):y+h+1, max(0, x-1):x+w+1] = 1
        integral = np.pad(occupied, ((1, 0), (1, 0))).cumsum(0).cumsum(1)
        counts = (integral[height:, width:] - integral[:-height, width:] -
                  integral[height:, :-width] + integral[:-height, :-width])
        choices = np.argwhere(counts == 0)
        if len(choices):
            y, x = choices[0]
            return page, int(x) + 1, int(y) + 1
    raise ValueError('Mantis shot has no free rectangle in the existing texture allocation')


def reference_sprite(directory):
    """Keep the connected ring, removing background and isolated original motes."""
    source = directory / 'frame-780-full.png'
    if hashlib.sha256(source.read_bytes()).hexdigest() != REFERENCE_SHA256:
        raise ValueError('Mantis projectile reference changed')
    paths = sorted(directory.glob('frame-*-full.png'))
    if len(paths) != 13:
        raise ValueError('Mantis projectile recovery requires the 13 aligned references')
    box = (115, 658, 519, 801)
    images = []
    for path in paths:
        with Image.open(path) as image:
            if image.size != (1920, 1080):
                raise ValueError('Mantis projectile recovery requires native original frames')
            images.append(np.asarray(image.convert('RGB').crop(box), dtype=np.float32))
    background = np.median(np.stack(images), axis=0)
    with Image.open(source) as image:
        shot = np.asarray(image.convert('RGB').crop(box), dtype=np.float32)
    white_delta = 255.0 - background
    alpha = np.clip(np.sum((shot - background) * white_delta, axis=2) /
                    np.maximum(1.0, np.sum(white_delta * white_delta, axis=2)), 0, 1)
    # The native shot was already composited with gate bars, bloom and motes.
    # A horizontal median removes their narrow marks from this approximate
    # matte while keeping the long ring arcs. The native alpha is still missing.
    padded = np.pad(alpha, ((0, 0), (12, 12)), mode='edge')
    alpha = np.median(np.lib.stride_tricks.sliding_window_view(padded, 25, axis=1), axis=-1)
    alpha = np.asarray(Image.fromarray(np.rint(alpha * 255).astype(np.uint8)).filter(
        ImageFilter.GaussianBlur(1.2)), dtype=np.float32) / 255
    core = alpha > .15
    height, width = core.shape
    connected = np.zeros_like(core)
    seen = np.zeros_like(core)
    for y, x in zip(*np.nonzero(core)):
        if seen[y, x]:
            continue
        pending = deque([(int(x), int(y))])
        seen[y, x] = True
        component = []
        while pending:
            px, py = pending.popleft()
            component.append((px, py))
            for nx, ny in ((px-1, py), (px+1, py), (px, py-1), (px, py+1)):
                if 0 <= nx < width and 0 <= ny < height and core[ny, nx] and not seen[ny, nx]:
                    seen[ny, nx] = True
                    pending.append((nx, ny))
        if len(component) >= 300:
            for px, py in component:
                connected[py, px] = True
    if np.count_nonzero(connected) < 3000:
        raise ValueError('Mantis ring silhouette was not recovered')
    support = np.asarray(Image.fromarray(connected.astype('uint8') * 255).filter(ImageFilter.MaxFilter(9))) > 0
    rgba = np.full((height, width, 4), 255, dtype=np.uint8)
    rgba[:, :, 3] = np.rint(alpha * support * 255).astype(np.uint8)
    # Hypothesis: native capture pixels map to the source's 64 pixels/unit on
    # the hero plane. Keep the capture-derived pivot/size explicit for review.
    geometry = (-width / 128, -height / 128, width / 128, height / 128)
    return Image.fromarray(rgba), geometry


def patch_cached_bank(output, references, tex3ds):
    """Add only the recovered shot; preserve every resident actor pixel/frame."""
    output = output.absolute()
    if not output.resolve().is_relative_to((ROOT / '_private/converted').resolve()):
        raise ValueError('Mantis bank must remain in this checkout private converted data')
    staging, backup = recover_publication(output)
    manifest = json.loads((output / 'manifest.json').read_text())
    source_clip = next(c for c in manifest['source_clips']['Lord'] if c['name'] == 'Mantis Air Projectile')
    if manifest['build_id'] != 22529139 or [f['spriteId'] for f in source_clip['frames']] != [23, 24, 7]:
        raise ValueError('Unexpected Mantis source bank')
    raw = (output / 'romfs/mantis-art.bin').read_bytes()
    magic, page_count, sprite_count, clip_count, frame_count = struct.unpack_from('<4s4I', raw)
    clip_start = 20 + 36 * sprite_count
    frame_start = clip_start + 68 * clip_count
    if magic != b'HKA1' or len(raw) != frame_start + 4 * frame_count:
        raise ValueError('Invalid resident Mantis bank')
    clips = [struct.unpack_from('<48sf4I', raw, clip_start + 68*i) for i in range(clip_count)]
    frame_ids = list(struct.unpack_from(f'<{frame_count}I', raw, frame_start))
    existing = next((c for c in clips if c[0].split(b'\0')[0].decode() == CLIP), None)
    index = frame_ids[existing[-2]] if existing else sprite_count
    if existing and set(frame_ids[existing[-2]:existing[-2]+existing[-1]]) != {index}:
        raise ValueError('Refusing to overwrite native projectile frames with the fallback')
    if existing and not manifest.get('reference_projectile'):
        raise ValueError('Refusing to overwrite native projectile art with the fallback')
    pages = [Image.open(output / f'mantis-page-{i}.png').convert('RGBA') for i in range(page_count)]
    records = [struct.unpack_from('<5I4f', raw, 20 + 36*i) for i in range(sprite_count)]
    tiles = []
    for page, x, y, width, height, *geometry in records:
        tiles.append((pages[page].crop((x, y, x+width, y+height)), tuple(geometry)))
    before = [hashlib.sha256(bitmap.tobytes()).hexdigest() for bitmap, _ in tiles]
    bitmap, geometry = reference_sprite(references)
    if existing:
        page, x, y, width, height, *_ = records[index]
        if bitmap.size != (width, height):
            raise ValueError('Captured shot changed size; preserve the existing actor atlas')
        records[index] = (page, x, y, width, height, *geometry)
        tiles[index] = bitmap, geometry
    else:
        page, x, y = free_slot(pages, records, bitmap.size)
        records.append((page, x, y, bitmap.width, bitmap.height, *geometry))
        tiles.append((bitmap, geometry))
        clips.append((CLIP.encode(), source_clip['fps'], source_clip['wrapMode'],
                      source_clip['loopStart'], len(frame_ids), 3))
        frame_ids.extend([index] * 3)
    pages[page].paste(bitmap, (x, y))
    changed_page = page
    baked_pages = pages
    for i, (record, (original, geometry)) in enumerate(zip(records, tiles)):
        if i == index:
            continue
        page, x, y, width, height, *packed_geometry = record
        actual = baked_pages[page].crop((x, y, x+width, y+height))
        if hashlib.sha256(actual.tobytes()).hexdigest() != before[i] or tuple(packed_geometry) != geometry:
            raise ValueError('Mantis shot changed resident actor art')
    append_floor_extension(clips, frame_ids, manifest)
    result = struct.pack('<4s4I', magic, page_count, len(tiles), len(clips), len(frame_ids))
    result += b''.join(struct.pack('<5I4f', *record) for record in records)
    result += b''.join(struct.pack('<48sf4I', *clip) for clip in clips)
    result += struct.pack(f'<{len(frame_ids)}I', *frame_ids)
    manifest.update(pages=page_count, sprites=len(tiles), reference_projectile={
        'source': str(references / 'frame-780-full.png'), 'sha256': REFERENCE_SHA256,
        'method': 'Temporal background median; horizontal matte median; connected white ring alpha; one captured pose repeated.',
        'geometry_hypothesis': '64 native capture pixels per world unit; centered pivot.',
        'original_clip': 'Mantis Air Projectile', 'native_sprite_ids': [23, 24, 7],
        'limitation': 'Capture-derived approximation. Native three-frame art/alpha/pivot still required.'})
    shutil.copytree(output, staging)
    for i in (changed_page,):
        page = baked_pages[i]
        png = staging / f'mantis-page-{i}.png'
        page.save(png)
        # This one-page fallback stays local; publishing its temporary art to
        # the shared conversion cache adds contention without reusable inputs.
        subprocess.run([str(tex3ds), '-f', 'rgba4', '-z', 'lz11', '-o',
                        str(staging / f'romfs/mantis-page-{i}.t3x'), str(png)], check=True, capture_output=True)
    (staging / 'romfs/mantis-art.bin').write_bytes(result)
    (staging / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    publish(output, staging, backup)
    print(f'Mantis captured ring: {bitmap.size}, sprite {index}, {page_count} pages; resident actor pixels preserved')
