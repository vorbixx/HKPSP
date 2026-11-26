"""Pack a complete room once, preserving pixels, filtering borders and draw order.

The admission budget is a platform limit, never a camera route. Oversized rooms
fail conversion instead of silently falling back to blocking gameplay streaming.
"""
import converted_files  # Install hardlink copy-on-write before asset writes.
import argparse
import hashlib
import json
import math
import struct
import subprocess
import tex3ds_cache
from pathlib import Path

from PIL import Image
from room_filtering import nearest_rgba4
from tool_paths import TEX3DS as TEX3DS_EXE

PAGE = 1024
PAGE_BYTES = PAGE * PAGE * 2


class RoomTextureBudgetError(ValueError):
    """Valid room art exceeds the native texture page allowance."""

def pack_rectangles(sizes, page_size=PAGE, *, width_first=False,
                    height_first=False, perimeter_first=False, score='short'):
    """Deterministic non-rotating MaxRects; indices retain their source identity."""
    if sum((width_first, height_first, perimeter_first)) > 1:
        raise ValueError('Select only one rectangle order')
    if score not in ('short', 'area', 'long'):
        raise ValueError('Unknown rectangle placement score')
    free = []
    placed = [None] * len(sizes)
    if width_first:
        order = lambda i: (-sizes[i][0], -sizes[i][1], i)
    elif height_first:
        order = lambda i: (-sizes[i][1], -sizes[i][0], i)
    elif perimeter_first:
        order = lambda i: (-(sizes[i][0]+sizes[i][1]), -max(sizes[i]), i)
    else:
        order = lambda i: (-max(sizes[i]), -sizes[i][0]*sizes[i][1], i)
    for index in sorted(range(len(sizes)), key=order):
        w, h = sizes[index]
        if min(w, h) <= 0 or max(w, h) > page_size:
            raise ValueError('Sprite including its filtering border exceeds a page')
        candidates = []
        for p, x, y, fw, fh in free:
            if w > fw or h > fh:
                continue
            dw, dh = fw-w, fh-h
            if score == 'area':
                fit = (dw*dh, min(dw, dh), max(dw, dh))
            elif score == 'long':
                fit = (max(dw, dh), min(dw, dh))
            else:
                fit = (min(dw, dh), max(dw, dh))
            candidates.append((*fit, p, y, x))
        if candidates:
            *_, page, y, x = min(candidates)
        else:
            page = max((p[0] for p in placed if p is not None), default=-1) + 1
            x = y = 0
            free.append((page, 0, 0, page_size, page_size))
        placed[index] = (page, x, y)
        split = []
        for p, fx, fy, fw, fh in free:
            if p != page or x >= fx+fw or x+w <= fx or y >= fy+fh or y+h <= fy:
                split.append((p, fx, fy, fw, fh))
                continue
            if x > fx: split.append((p, fx, fy, x-fx, fh))
            if x+w < fx+fw: split.append((p, x+w, fy, fx+fw-x-w, fh))
            if y > fy: split.append((p, fx, fy, fw, y-fy))
            if y+h < fy+fh: split.append((p, fx, y+h, fw, fy+fh-y-h))
        unique = sorted(set(split))
        # Rectangles on different pages cannot contain each other. Keep the
        # original sorted order and tie-breaking while avoiding cross-page scans.
        by_page = {}
        for rectangle in unique:
            by_page.setdefault(rectangle[0], []).append(rectangle)
        free = [r for rectangles in by_page.values() for r in rectangles if not any(
            r != q and q[1] <= r[1] and q[2] <= r[2]
            and r[1]+r[3] <= q[1]+q[3] and r[2]+r[4] <= q[2]+q[4]
            for q in rectangles)]
    return placed


def room_header(data):
    """Validate the room layout and sparse draw bindings before repacking."""
    if len(data) < 20 or data[:4] not in (b'HKR2', b'HKR3', b'HKR4'):
        raise ValueError('Expected HKR2/HKR3/HKR4 room artwork')
    pages, count, draw_count, reserved = struct.unpack_from('<4I', data, 4)
    if data[:4] == b'HKR2' and reserved:
        raise ValueError('Invalid room header')
    end = 20+count*36+draw_count*72
    if len(data) != end+reserved*24+(4 if data[:4] == b'HKR4' else 0):
        raise ValueError('Invalid room metadata length')
    if data[:4] == b'HKR4':
        split, = struct.unpack_from('<f', data, end+reserved*24)
        low, high = struct.unpack('<2f', struct.pack('<2f', -28.1, 961.9))
        if not math.isfinite(split) or not low <= split <= high:
            raise ValueError('Invalid room blur boundary')
    seen = set()
    for i in range(reserved):
        draw, *values = struct.unpack_from('<I5f', data, end+i*24)
        if draw >= draw_count or draw in seen or not all(math.isfinite(v) and abs(v) <= 100000 for v in values):
            raise ValueError('Invalid grass sway binding')
        seen.add(draw)
    return pages, count, draw_count, reserved


def switch_sprites(data, binary, max_depth):
    """Retain animation-only sprites for owners admitted by the Old depth window."""
    if not binary:
        return set()
    if len(binary) < 8 or binary[:4] != b'MGA1':
        raise ValueError('Invalid switch animation bank')
    _, sprites, draws, _ = room_header(data)
    count, = struct.unpack_from('<I', binary, 4)
    offset = 8
    retained = set()
    for _ in range(count):
        _, slots, frames, _ = struct.unpack_from('<3If', binary, offset)
        if not 1 <= slots <= 4 or not 1 <= frames <= 256:
            raise ValueError('Invalid switch animation counts')
        indices = struct.unpack_from('<4I', binary, offset+16)
        initial = struct.unpack_from('<4I', binary, offset+32)
        offset += 48
        samples = [initial]
        for _ in range(frames):
            samples.append(struct.unpack_from('<f4I', binary, offset)[1:])
            offset += 20
        for slot, draw in enumerate(indices[:slots]):
            if draw >= draws:
                raise ValueError('Invalid switch animation draw')
            depth, = struct.unpack_from('<f', data, 20+sprites*36+draw*72+32)
            for sample in samples:
                sprite = sample[slot]
                if sprite != 0xffffffff:
                    if sprite >= sprites:
                        raise ValueError('Invalid switch animation sprite')
                    if abs(depth) < max_depth:
                        retained.add(sprite)
    if offset != len(binary):
        raise ValueError('Invalid switch animation bank length')
    return retained


def repack(data, images, max_bytes, *, max_depth=None, switches=b''):
    pages, count, draw_count, reserved = room_header(data)
    if not count or not draw_count or pages != len(images):
        raise ValueError('Invalid room header')
    if any(image.size != (PAGE, PAGE) or image.mode != 'RGBA' for image in images):
        raise ValueError('Expected complete RGBA source atlas pages')
    sprites = [struct.unpack_from('<5I4f', data, 20+i*36) for i in range(count)]
    for page, x, y, w, h, left, bottom, right, top in sprites:
        if page >= pages or min(x, y, w, h) < 1 or x+w >= PAGE or y+h >= PAGE:
            raise ValueError('Missing sprite filtering border')
        if not all(math.isfinite(v) and abs(v) <= 100000 for v in (left,bottom,right,top)) or not (left < right and bottom < top):
            raise ValueError('Invalid sprite geometry')
    for i in range(draw_count):
        sprite, owner, *values = struct.unpack_from('<2I16f', data, 20+count*36+i*72)
        if sprite >= count or values[15] not in (0, 1, 2, 3, 4, 5) or not all(math.isfinite(v) and abs(v) <= 100000 for v in values) or not all(0 <= v <= 1 for v in values[7:11]):
            raise ValueError('Invalid room draw')
    if max_depth is not None:
        # Retain draw/sprite identity for grass, plants and scripted owners. Only
        # omitted layers use a shared transparent texel; retained art is lossless.
        retained = {struct.unpack_from('<I', data, 20+count*36+i*72)[0]
                    for i in range(draw_count)
                    if abs(struct.unpack_from('<f', data, 20+count*36+i*72+32)[0]) < max_depth}
        retained.update(switch_sprites(data, switches, max_depth))
        placeholder = len(images)
        images = images + [Image.new('RGBA', (PAGE, PAGE))]
        sprites = [s if i in retained else (placeholder, 1, 1, 1, 1, *s[5:])
                   for i, s in enumerate(sprites)]
    # Sprites with exactly the same bordered texels can share one atlas slot.
    # Keep every metadata entry and draw index: aliases only affect the sampled
    # rectangle, so per-sprite geometry, materials and ordering remain intact.
    unique_sizes = []
    canonical = []
    fingerprints = {}
    for source_index, sprite in enumerate(sprites):
        page, x, y, w, h = sprite[:5]
        tile = images[page].crop((x-1, y-1, x+w+1, y+h+1))
        pixels = tile.tobytes()
        key = (w+2, h+2, hashlib.sha256(pixels).digest())
        match = next((unique_index for unique_index, original_index in fingerprints.get(key, ())
                      if images[sprites[original_index][0]].crop((sprites[original_index][1]-1,
                                                                  sprites[original_index][2]-1,
                                                                  sprites[original_index][1]+sprites[original_index][3]+1,
                                                                  sprites[original_index][2]+sprites[original_index][4]+1)).tobytes() == pixels), None)
        if match is None:
            match = len(unique_sizes)
            unique_sizes.append((w+2, h+2))
            fingerprints.setdefault(key, []).append((match, source_index))
        canonical.append(match)
    unique_positions = pack_rectangles(unique_sizes)
    positions = [unique_positions[i] for i in canonical]
    page_count = 1+max(p[0] for p in positions)
    if page_count*PAGE_BYTES > max_bytes:
        # Keep the current layout whenever it fits. Only over-budget rooms try
        # alternate deterministic MaxRects orders and scores; choose the layout
        # with the fewest pages, retaining the earlier result on ties.
        for options in ({'width_first': True}, {'height_first': True},
                        {'perimeter_first': True},
                        {'height_first': True, 'score': 'area'}):
            alternative = pack_rectangles(unique_sizes, **options)
            alternative_pages = 1+max(p[0] for p in alternative)
            if alternative_pages < page_count:
                positions, page_count = [alternative[i] for i in canonical], alternative_pages
    if page_count*PAGE_BYTES > max_bytes:
        raise RoomTextureBudgetError(f'Room needs {page_count*PAGE_BYTES} texture bytes; budget {max_bytes}')
    atlases = [Image.new('RGBA', (PAGE, PAGE)) for _ in range(page_count)]
    metadata = bytearray(struct.pack('<4s4I', data[:4], page_count, count, draw_count, reserved))
    for i, (s, (page, x, y)) in enumerate(zip(sprites, positions)):
        old_page, sx, sy, w, h = s[:5]
        atlases[page].paste(images[old_page].crop((sx-1, sy-1, sx+w+1, sy+h+1)), (x, y))
        metadata += struct.pack('<5I', page, x+1, y+1, w, h)
        # Preserve the original floating point bits, including local geometry.
        metadata += data[20+i*36+20:20+(i+1)*36]
    metadata += data[20+count*36:]
    return bytes(metadata), atlases


def resident_page_formats(metadata, atlases, max_bytes):
    """Keep smooth constant-colour alpha ramps in RGBA8 within the room cap.

    RGBA4 has only 16 alpha levels: stretching a source fog ramp magnifies those
    steps into horizontal bands. Other artwork retains the existing RGBA4 path.
    """
    count = struct.unpack_from('<I', metadata, 8)[0]
    gradients = set()
    for i in range(count):
        page, x, y, w, h = struct.unpack_from('<5I', metadata, 20+i*36)
        tile = atlases[page].crop((x, y, x+w, y+h))
        extrema = tile.getextrema()
        if (all(low == high and low > 0 for low, high in extrema[:3])
                and len(tile.getchannel('A').getcolors(256)) > 16):
            gradients.add(page)
    # Each promotion costs one additional RGBA4 page. Keep admission fixed;
    # rooms without spare capacity retain their previous quantization explicitly.
    available = max(0, max_bytes//PAGE_BYTES-len(atlases))
    promoted = set(sorted(gradients)[:available])
    return ['rgba8' if i in promoted else 'rgba4' for i in range(len(atlases))], sorted(gradients-promoted)


def convert(source, output, romfs, max_bytes, tex3ds, *, required_texture_scale=None,
            old_actor_texture_bytes=0):
    source_manifest=source/'manifest.json'
    texture_scale=json.loads(source_manifest.read_text()).get('texture_scale') if source_manifest.exists() else None
    if required_texture_scale is not None and texture_scale!=required_texture_scale:
        raise ValueError(f'Resident source texture scale {texture_scale} must be {required_texture_scale}')
    data = (source/'room-visual.bin').read_bytes()
    pages, _, _, _ = room_header(data)
    inputs = [source/'room-visual.bin']+[source/f'room-page-{i}.png' for i in range(pages)]
    grass_source = source/'grass.bin'
    images = [Image.open(p).convert('RGBA') for p in inputs[1:]]
    metadata, atlases = repack(data, images, max_bytes)
    switch_source = source/'room-visual.bin.switches'
    switches = switch_source.read_bytes() if switch_source.is_file() else b''
    if switches:
        inputs.append(switch_source)
    formats, deferred_gradients = resident_page_formats(metadata, atlases, max_bytes)
    grass_binary = None
    if grass_source.is_file():
        inputs.append(grass_source)
        grass_binary = bytearray(grass_source.read_bytes())
        if len(grass_binary) < 16 or grass_binary[:4] != b'HKG1':
            raise ValueError('Invalid source grass bank')
        count, source_sprites, source_draws = struct.unpack_from('<3I', grass_binary, 4)
        room_sprites, room_draws = struct.unpack_from('<II', data, 8)
        resident_sprites, resident_draws = struct.unpack_from('<II', metadata, 8)
        record_bytes = 8*4+32*4
        if count != 41 or source_sprites != room_sprites or source_draws != room_draws or \
                len(grass_binary) != 16+count*record_bytes:
            raise ValueError('Grass bank does not match source room geometry')
        # Repacking retains sprite and draw identity order; only atlas locations and
        # resident counts change. Keep a byte-level guard on the unchanged draw table.
        if (room_sprites != resident_sprites or room_draws != resident_draws or
                data[20+room_sprites*36:] != metadata[20+resident_sprites*36:]):
            raise ValueError('Resident room changed grass sprite or draw identity')
        struct.pack_into('<II', grass_binary, 8, resident_sprites, resident_draws)
    lever_source = source/'room-levers.bin'
    lever_binary = None
    if lever_source.is_file():
        inputs.append(lever_source)
        lever_binary = lever_source.read_bytes()
        if len(lever_binary) < 8 or lever_binary[:4] not in (b'HKA1',b'HKA2'):
            raise ValueError('Invalid source gate artwork bank')
        # Gate animation frames reference sprite IDs in this same resident bank.
        sprite_count = room_header(data)[1]
        if (data[8:16] != metadata[8:16] or
                data[20+sprite_count*36:] != metadata[20+sprite_count*36:]):
            raise ValueError('Resident room changed gate sprite or draw identity')
    shroom_source = source/'room-shrooms.bin'
    shroom_binary = None
    if shroom_source.is_file():
        from scene_shrooms import resident_artwork
        inputs.append(shroom_source)
        shroom_binary = resident_artwork(shroom_source.read_bytes(), data, metadata)
    plant_binding = None
    if (source/'plant-artwork.json').is_file():
        from plant_artwork import resident_binding
        inputs.append(source/'plant-artwork.json')
        source_manifest = json.loads((source/'manifest.json').read_bytes())
        if hashlib.sha256(inputs[-1].read_bytes()).hexdigest() != source_manifest.get('plant_artwork_sha256'):
            raise ValueError('Changed plant artwork binding')
        plant_binding = resident_binding(json.loads(inputs[-1].read_bytes()), data, metadata)
    dynamics_binding = None
    if (source/'room-dynamics.json').is_file():
        from room_dynamics_artwork import compile_bank, digest
        inputs.append(source/'room-dynamics.json')
        source_manifest = json.loads((source/'manifest.json').read_bytes())
        if digest(inputs[-1].read_bytes()) != source_manifest.get('room_dynamics_sha256'):
            raise ValueError('Changed dynamics artwork binding')
        dynamics_binding = json.loads(inputs[-1].read_bytes())
        compile_bank(dynamics_binding, data)
        dynamics_binding['room_visual_sha256'] = digest(metadata)
        dynamics_binary = compile_bank(dynamics_binding, metadata)
    output.mkdir(parents=True, exist_ok=True)
    romfs.mkdir(parents=True, exist_ok=True)
    for i, atlas in enumerate(atlases):
        png = output/f'room-resident-page-{i}.png'
        texture = romfs/f'room-resident-page-{i}.t3x'
        atlas.save(png)
        encoded_png = output/f'room-resident-page-{i}-encoded.png'
        encoded = atlas if formats[i] == 'rgba8' else nearest_rgba4(atlas)
        encoded.save(encoded_png)
        tex3ds_cache.run([str(tex3ds), '-f', formats[i], '-z', 'lz11', '-o', str(texture), str(encoded_png)],
                       check=True, stdout=subprocess.DEVNULL)
    (romfs/'room-resident-visual.bin').write_bytes(metadata)
    switch_target = romfs/'room-resident-visual.bin.switches'
    if switches:
        switch_target.write_bytes(switches)
    else:
        switch_target.unlink(missing_ok=True)
    # Owner bindings survive resident and Old sprite/draw index remapping.
    scale_source=source/'room-visual.bin.scale'
    for name in ('room-resident-visual.bin.scale','room-old-resident-visual.bin.scale'):
        scale_target=romfs/name
        if scale_source.is_file():scale_target.write_bytes(scale_source.read_bytes())
        else:scale_target.unlink(missing_ok=True)
    # Use the encoded alpha support, including atlas borders, for fog-mesh.flag.
    # Scene converters also use this path before publishing their staging bank.
    from fog_strips import write as write_strips
    _, strips = write_strips(romfs/'room-resident-visual.bin', output, 'room-resident-page-{}-encoded.png')
    print(f'Opaque-support strips: {len(strips)} sprites')
    # Separate Old atlases keep New texture/metadata bytes unchanged. No art is
    # downsampled: only layers outside the table's native-depth window disappear.
    from check_memory_budget import old_profile_knobs, old_profile_depths, _cpp_constants, ROOT
    depth, old_prefix = old_profile_knobs()
    requested_depth = depth
    old_budget = _cpp_constants(ROOT/'include/render_memory.hpp')['oldRoomTextureBudget']
    if old_actor_texture_bytes<0:raise ValueError('Negative Old actor texture bytes')
    old_limit = max(0,old_budget-old_actor_texture_bytes)
    for stale in romfs.glob(f'{old_prefix}page-*.t3x'):
        stale.unlink()
    (romfs/f'{old_prefix}visual.bin').unlink(missing_ok=True)
    (romfs/f'{old_prefix}visual.bin.switches').unlink(missing_ok=True)
    old_atlases = None
    for depth in dict.fromkeys(old_profile_depths()):
        try:
            old_metadata, old_atlases = repack(data, images, old_limit, max_depth=depth, switches=switches)
            break
        except RoomTextureBudgetError as error:
            # Only admission failures may omit Old; malformed art still fails.
            print(f'Old room depth {depth}: {error}')
    if old_atlases is not None:
        for i, atlas in enumerate(old_atlases):
            png = output/f'{old_prefix}page-{i}-rgba4.png'
            nearest_rgba4(atlas).save(png)
            tex3ds_cache.run([str(tex3ds), '-f', 'rgba4', '-z', 'lz11', '-o',
                             str(romfs/f'{old_prefix}page-{i}.t3x'), str(png)],
                            check=True, stdout=subprocess.DEVNULL)
        (romfs/f'{old_prefix}visual.bin').write_bytes(old_metadata)
        if switches:
            (romfs/f'{old_prefix}visual.bin.switches').write_bytes(switches)
        print(f'Old room: {len(old_atlases)} pages, {len(old_atlases)*PAGE_BYTES} GPU bytes; native retained pixels')

    ambient_source=source/'room-ambient.bin'
    if ambient_source.is_file():
        ambient=ambient_source.read_bytes()
        if len(ambient)<24 or ambient[:4]!=b'HKA1' or struct.unpack_from('<II',ambient,16)!=struct.unpack_from('<II',metadata,8):
            raise ValueError('Tablet/Glow Bug bank does not match resident room geometry')
        (output/'room-ambient.bin').write_bytes(ambient)
        (romfs/'room-ambient.bin').write_bytes(ambient)
    if grass_binary is not None:
        (output/'grass.bin').write_bytes(grass_binary)
        (romfs/'grass.bin').write_bytes(grass_binary)
    else:
        (output/'grass.bin').unlink(missing_ok=True)
        (romfs/'grass.bin').unlink(missing_ok=True)
    if dynamics_binding is not None:
        (output/'room-dynamics.json').write_text(json.dumps(dynamics_binding, indent=2)+'\n')
        (output/'room-dynamics.bin').write_bytes(dynamics_binary)
        (romfs/'room-dynamics.bin').write_bytes(dynamics_binary)
    else:
        (output/'room-dynamics.json').unlink(missing_ok=True)
        (output/'room-dynamics.bin').unlink(missing_ok=True)
        (romfs/'room-dynamics.bin').unlink(missing_ok=True)
    for directory in (output, romfs):
        if shroom_binary is None:
            (directory/'room-shrooms.bin').unlink(missing_ok=True)
        else:
            (directory/'room-shrooms.bin').write_bytes(shroom_binary)
        if lever_binary is None:
            (directory/'room-levers.bin').unlink(missing_ok=True)
        else:
            (directory/'room-levers.bin').write_bytes(lever_binary)
    if plant_binding is not None:
        (output/'plant-artwork.json').write_text(json.dumps(plant_binding, indent=2)+'\n', encoding='utf-8')
    else:
        (output/'plant-artwork.json').unlink(missing_ok=True)
    manifest = dict(format='mixed' if 'rgba8' in formats else 'rgba4', texture_formats=formats, deferred_alpha_gradient_pages=deferred_gradients, alpha_quantization='per-page', color_quantization='per-page', texture_scale=texture_scale, pages=len(atlases), texture_bytes=sum(PAGE_BYTES*(2 if fmt == 'rgba8' else 1) for fmt in formats),
                    budget_bytes=max_bytes, sprites=struct.unpack_from('<I', data, 8)[0],
                    draws=struct.unpack_from('<I', data, 12)[0],
                    source_sha256={p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs},
                    output_sha256={p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                                   for p in [romfs/'room-resident-visual.bin']+
                                   [romfs/f'room-resident-page-{i}.t3x' for i in range(len(atlases))]+
                                   ([romfs/'room-levers.bin'] if lever_binary is not None else [])+
                                   ([romfs/'room-shrooms.bin'] if shroom_binary is not None else [])})
    manifest['old_profile'] = (dict(max_depth=depth, available=False, reason='exceeds the Old texture budget')
        if old_atlases is None else
        dict(max_depth=depth, available=True, pages=len(old_atlases),
        output_sha256={p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                       for p in [romfs/f'{old_prefix}visual.bin'] +
                       [romfs/f'{old_prefix}page-{i}.t3x' for i in range(len(old_atlases))]}))
    manifest['old_profile']['requested_max_depth'] = requested_depth
    manifest['old_profile'].update(actor_texture_bytes=old_actor_texture_bytes,
                                   budget_bytes=old_budget,room_budget_bytes=old_limit)
    if plant_binding is not None:
        manifest['plant_artwork_sha256'] = hashlib.sha256((output/'plant-artwork.json').read_bytes()).hexdigest()
    (output/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    print(f'Room: {len(atlases)} pages, {manifest["texture_bytes"]} GPU bytes; no gameplay streaming')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--romfs', type=Path, required=True)
    parser.add_argument('--max-texture-bytes', type=int, default=16*1024*1024)
    parser.add_argument('--required-texture-scale', type=float)
    parser.add_argument('--tex3ds', type=Path, default=Path(TEX3DS_EXE))
    args = parser.parse_args()
    convert(args.source, args.output, args.romfs, args.max_texture_bytes, args.tex3ds,required_texture_scale=args.required_texture_scale)
