"""Build an opt-in HKR3/HKR4 static-decor bank from conservative draw bands.

Only owners explicitly admitted by the conversion manifest are considered.  A
group must be consecutive, axis aligned, identically tinted and depth sorted;
groups that fail the exact RGBA4 tile test remain in the source bank.  The
result keeps all dynamic draws and replaces each accepted group's members with
one opaque-capable atlas sprite, so covered members are culled at conversion.
"""
import argparse
import json
import shutil
import struct
import subprocess
import hashlib
from pathlib import Path

from PIL import Image

from fog_composite import candidates, exact_tile_composite, load_bank, prototype_bank
from check_memory_budget import actor_texture_bytes
from tool_paths import TEX3DS as TEX3DS_EXE

ROOT = Path(__file__).resolve().parents[1]
PAGE_SIZE = 1024
PAGE_BYTES = 2 * 1024 * 1024
# Keep this in sync with include/render_memory.hpp and include/room_visual.hpp:
# roomTextureBudget / roomTexturePageBytes, plus the admission reserve.
ROOM_TEXTURE_BUDGET_BYTES = 28 * 1024 * 1024
ROOM_LINEAR_RESERVE_BYTES = 2 * 1024 * 1024
ROOM_PAGE_LIMIT = ROOM_TEXTURE_BUDGET_BYTES // PAGE_BYTES


def source_page_units(page_directory, bank_directory, page_prefix, count):
    """Runtime admission size of the source pages, in 2 MiB units.

    RoomRenderer charges a page by its t3x format byte: RGBA8 (0) occupies two
    units, RGBA4 one. Counting files instead admitted Town (13 files, 14 units)
    one composite page over the 28 MiB budget (console load error 2026-11-10).
    """
    units = 0
    for index in range(count):
        t3x = page_directory / f'{page_prefix}{index}.t3x'
        if not t3x.exists():
            t3x = bank_directory / f'{page_prefix}{index}.t3x'
        header = t3x.read_bytes()[:4] if t3x.exists() else b''
        units += 2 if len(header) == 4 and header[3] == 0 else 1
    return units


def _owners(args):
    result = set(args.owners or ())
    if args.owners_file:
        value = json.loads(args.owners_file.read_text(encoding='utf-8'))
        if isinstance(value, dict):
            value = value.get('owners', ())
        if not isinstance(value, list):
            raise ValueError('owners file must contain an array or {"owners": [...]}')
        result.update(int(owner) for owner in value)
    if not result or any(owner < 0 or owner > 0xffffffff for owner in result):
        raise ValueError('at least one valid static owner is required')
    return result


def _pack(images):
    """Pack bordered composite images and return interior sprite positions.

    ``exact_tile_composite`` includes the one-pixel filtering border in each
    image.  Room-bank sprite rectangles point at the interior, so placements
    returned here are one pixel inside the pasted image.
    """
    pages = []
    placements = []
    x = y = row = 0
    original = list(images)
    indexed = sorted(enumerate(images), key=lambda pair: -pair[1].height)
    for _, image in indexed:
        if image.width < 3 or image.height < 3 or image.width > PAGE_SIZE or image.height > PAGE_SIZE:
            raise ValueError('composite exceeds 1022-pixel resident atlas limit')
        if not pages:
            pages.append(Image.new('RGBA', (PAGE_SIZE, PAGE_SIZE)))
        if x + image.width > PAGE_SIZE:
            x, y, row = 0, y + row + 1, 0
        if y + image.height > PAGE_SIZE:
            pages.append(Image.new('RGBA', (PAGE_SIZE, PAGE_SIZE)))
            x, y, row = 0, 0, 0
        page = len(pages) - 1
        pages[page].paste(image, (x, y))
        placements.append((page, x + 1, y + 1))
        x += image.width + 1
        row = max(row, image.height)
    reordered = [None] * len(original)
    for (index, _), placement in zip(indexed, placements):
        reordered[index] = placement
    return pages, reordered


def source_texture_size(page_directory, bank_directory, page_prefix, page_count):
    """Account for RGBA8 source pages as well as the usual RGBA4 pages."""
    total = 0
    for index in range(page_count):
        name = f'{page_prefix}{index}.t3x'
        path = page_directory / name
        if not path.exists():
            path = bank_directory / name
        with path.open('rb') as stream:
            header = stream.read(4)
        total += actor_texture_bytes(header)
    return total


def bake(bank, page_directory, output, owners, page_prefix='room-page-',
         same_owner_only=False, tex3ds=Path(TEX3DS_EXE), screen_scale=0,
         source=None, bindings_directory=None):
    sprites, entries, animated = load_bank(bank)
    data = bank.read_bytes()
    source_pages = struct.unpack_from('<I', data, 4)[0]
    source_textures = []
    for index in range(source_pages):
        texture = page_directory / f'{page_prefix}{index}.t3x'
        if not texture.exists():
            texture = bank.parent / f'{page_prefix}{index}.t3x'
        source_textures.append(texture)
    source_linear_bytes = source_texture_size(page_directory, bank.parent, page_prefix, source_pages)
    composite_page_limit = min(ROOM_PAGE_LIMIT - source_pages,
                               (ROOM_TEXTURE_BUDGET_BYTES - source_linear_bytes) // PAGE_BYTES)
    if composite_page_limit < 0:
        raise ValueError('source room atlas exceeds room texture budget')
    pages = {index: Image.open(page_directory / f'{page_prefix}{index}.png').convert('RGBA')
             for index in range(source_pages)}
    if source:
        from static_decor_manifest import classify
        from static_decor_bindings import protected_owners
        protected = {entries[index]['owner'] for index in animated}
        protected.update(protected_owners(page_directory))
        manifest = classify(source, {entry['owner'] for entry in entries}, protected)
        owners = set(manifest['owners'])
    else:
        manifest = dict(owners=sorted(owners))
    groups = candidates(entries, animated, owners, same_owner_only or bool(screen_scale))
    arrays = None
    if screen_scale:
        from static_decor_tiles import raster_tiles, texture_arrays, tile_bank
        arrays = texture_arrays(pages)
    accepted = []
    report = []
    for group in groups:
        row = dict(draws=[index for index, _, _ in group], owners=sorted({entry['owner'] for _, entry, _ in group}),
                   depth=group[0][2][0], layer=group[0][2][1], order=group[0][2][2])
        try:
            if screen_scale:
                tiles = raster_tiles(group, sprites, arrays, screen_scale)
                row.update(accepted=True, tiles=len(tiles), pixels=sum(im.width*im.height for im, _ in tiles))
                accepted.append((group, tiles))
                report.append(row)
                continue
            image, bounds = exact_tile_composite(group, sprites, pages)
            row.update(accepted=True, size=image.size, bounds=bounds)
            accepted.append((group, image, bounds))
        except ValueError as error:
            row.update(accepted=False, reason=str(error))
        report.append(row)
    if screen_scale:
        # Prefer the most draw reduction per texel. Rejected bands keep their
        # original source draws; the room's hard admission limit never changes.
        accepted.sort(key=lambda pair: -(len(pair[0])-len(pair[1])) /
                      sum(image.width*image.height for image, _ in pair[1]))
        chosen, images, area = [], [], 0
        for group, tiles in accepted:
            extra = [image for image, _ in tiles]
            if area + sum(im.width*im.height for im in extra) > composite_page_limit*PAGE_SIZE**2*.8:
                continue
            trial_pages, _ = _pack(images+extra)
            if len(trial_pages) <= composite_page_limit:
                chosen.append((group, tiles))
                images += extra
                area += sum(im.width*im.height for im in extra)
        accepted = sorted(chosen, key=lambda pair: pair[0][0][0])
        images = [image for _, tiles in accepted for image, _ in tiles]
    else:
        images = [image for _, image, _ in accepted]
    composite_pages, local = _pack(images) if images else ([], [])
    total_pages = source_pages + len(composite_pages)
    if source_pages > ROOM_PAGE_LIMIT or total_pages > ROOM_PAGE_LIMIT or len(composite_pages) > composite_page_limit:
        raise ValueError(f'static room atlas exceeds {ROOM_PAGE_LIMIT}-page room texture budget')
    placements = [(source_pages + page, x, y) for page, x, y in local]
    # The composite image contains the source texels without the draw tint.
    # Keep the authored tint on the replacement draw so the runtime applies it
    # exactly once, as it did for every source member.
    baked = (tile_bank(data, accepted, placements) if screen_scale else
             prototype_bank(data, accepted, placements, preserve_tint=True))
    output.mkdir(parents=True, exist_ok=True)
    (output / 'static-owners.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
    for index in range(source_pages):
        shutil.copyfile(page_directory / f'{page_prefix}{index}.png', output / f'static-room-page-{index}.png')
        shutil.copyfile(source_textures[index], output / f'static-room-page-{index}.t3x')
    if composite_pages and not tex3ds.exists():
        raise ValueError(f'tex3ds encoder not found: {tex3ds}')
    for local_index, page in enumerate(composite_pages):
        index = source_pages + local_index
        png = output / f'static-room-page-{index}.png'
        t3x = output / f'static-room-page-{index}.t3x'
        page.save(png)
        subprocess.run([str(tex3ds), '-f', 'rgba4', '-z', 'none', '-o', str(t3x), str(png)], check=True)
    (output / 'static-room-visual.bin').write_bytes(baked)
    if bindings_directory:
        # Original sprite records and draw indices are unchanged. Retarget only
        # the validated cardinalities, and check every bound runtime owner was
        # excluded before producing the opt-in copies of these banks.
        from static_decor_bindings import retarget_bindings
        retarget_bindings(bindings_directory, output, data, baked, page_directory)
    baked_linear_bytes = source_linear_bytes + len(composite_pages) * PAGE_BYTES
    result = dict(source_pages=source_pages, pages=total_pages,
                  accepted_groups=len(accepted), candidate_groups=len(groups),
                  source_texture_bytes=source_linear_bytes,
                  baked_texture_bytes=baked_linear_bytes,
                  source_linear_texture_bytes=source_linear_bytes,
                  baked_linear_texture_bytes=baked_linear_bytes,
                  source_linear_admission_bytes=source_linear_bytes + ROOM_LINEAR_RESERVE_BYTES,
                  baked_linear_admission_bytes=baked_linear_bytes + ROOM_LINEAR_RESERVE_BYTES,
                  source_vram_bytes=0,
                  baked_vram_bytes=0,
                  room_texture_budget_bytes=ROOM_TEXTURE_BUDGET_BYTES,
                  room_linear_reserve_bytes=ROOM_LINEAR_RESERVE_BYTES,
                  culled_draws=(sum(len(group)-len(tiles) for group, tiles in accepted) if screen_scale else
                                sum(len(group)-1 for group, _, _ in accepted)),
                  source_sha256=hashlib.sha256(data).hexdigest(),
                  baked_sha256=hashlib.sha256(baked).hexdigest(),
                  screen_scale=screen_scale,
                  baked_draw_indices=[index for row in accepted for index, _, _ in row[0]],
                  owners=sorted(owners), groups=report)
    (output / 'static-decor.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bank', type=Path, required=True)
    parser.add_argument('--page-directory', type=Path, default=None)
    parser.add_argument('--page-prefix', default='room-page-')
    parser.add_argument('--owners', type=int, nargs='*')
    parser.add_argument('--owners-file', type=Path)
    parser.add_argument('--source', type=Path, help='generate a conservative owner manifest from this export')
    parser.add_argument('--screen-scale', type=float, default=0, help='screen oversampling factor; 0 uses exact joins')
    parser.add_argument('--bindings-directory', type=Path)
    parser.add_argument('--same-owner-only', action='store_true')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--tex3ds', type=Path, default=Path(TEX3DS_EXE))
    args = parser.parse_args()
    output = args.output.resolve()
    if not any(output.is_relative_to((ROOT / parent).resolve()) for parent in ('build', '_private/converted')):
        parser.error('output must be under build or the worktree converted directory')
    if not 0 <= args.screen_scale <= 4:
        parser.error('screen-scale must be between 0 and 4')
    try:
        result = bake(args.bank, args.page_directory or args.bank.parent, output, set() if args.source else _owners(args),
                      args.page_prefix, args.same_owner_only, args.tex3ds, args.screen_scale,
                      args.source, args.bindings_directory)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        parser.error(str(error))
    print(json.dumps({key: result[key] for key in ('source_pages', 'pages', 'accepted_groups', 'culled_draws')}))


if __name__ == '__main__':
    main()
