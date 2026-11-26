"""Convert copied original Crossroads boss artwork to a private HKA1 bank.

Requires UnityPy 1.25.2 and TypeTreeGeneratorAPI (optionally --packages).
Aliases are presentation approximations; original clip timing/geometry is kept.
"""
import converted_files  # Install hardlink copy-on-write before asset writes.
import argparse
import copy
import hashlib
import json
import shutil
from pathlib import Path
import struct
import tex3ds_cache
import sys

from PIL import Image
from assetripper_client import ROOT
from atlas_packing import pack_pages
from convert_hero import decode_sprite, trim_transparent
from animation_library import complete_aliases, event_bytes
from tool_paths import TEX3DS as TEX3DS_EXE

ALIASES = {
    'Gruz': ('sharedassets32.assets', 765, {
        'Idle': 'Sleep', 'Wake': 'Wake', 'Fly': 'Fly', 'Charge': 'Charge',
        'Slam': 'Slam Down', 'Death': 'Death'}),
    'Knight': ('sharedassets47.assets', 9, {
        'Idle': 'Idle', 'Jump': 'Jump', 'Fall': 'Jump Attack Hit 1', 'Land': 'Land',
        'Jump Antic': 'Jump Antic', 'Attack Antic': 'Attack Antic',
        'Slam': 'Attack', 'Stagger': 'Stun Open', 'Recover': 'Stun Recover',
        'Rage': 'Rage', 'Death': 'Death Fall', 'Corpse': 'Death Spaz', 'Opened': 'Stun Opened'}),
    'Head': ('sharedassets47.assets', 9, {
        'Idle': 'Head Idle', 'Hit': 'Head Hit', 'Death': 'Head Spaz'}),
    'Gruzzer': ('sharedassets40.assets', 208, {'Fly': 'Fly'}),
}


def unity_sprite_geometry(sprite, size):
    """Keep a trimmed Unity Sprite at its original pivot and world scale."""
    rect, pivot, ppu = sprite.m_Rect, sprite.m_Pivot, sprite.m_PixelsToUnits
    offset = sprite.m_RD.textureRectOffset
    width, height = sprite.m_RD.textureRect.width, sprite.m_RD.textureRect.height
    if ppu <= 0 or width <= 0 or height <= 0 or size != (round(width), round(height)):
        raise ValueError('Invalid Unity sprite geometry')
    left = (offset.x - pivot.x * rect.width) / ppu
    bottom = (offset.y - pivot.y * rect.height) / ppu
    return left, bottom, left + width / ppu, bottom + height / ppu


def make_sprite_tiles(keys, sprites, split_oversized=False):
    """Return HKA1 tile sprites and source-index mappings, optionally splitting wide sprites."""
    max_extent = 1022  # One transparent border pixel on both sides of a 1024 page.
    tiles = [sprites[key] for key in keys]
    source_parts = [[index, 0xffffffff] for index in range(len(keys))]
    for index, key in enumerate(keys):
        bitmap, geometry = sprites[key]
        width, height = bitmap.size
        if width <= max_extent and height <= max_extent:
            continue
        if not split_oversized:
            raise ValueError(f'Sprite exceeds 1024 atlas: {key} ({width}x{height})')
        if height > max_extent or width > max_extent * 2:
            raise ValueError(f'Sprite cannot be represented by at most two horizontal tiles: {key} ({width}x{height})')
        first_width = max_extent
        left, bottom, right, top = geometry
        split_world_x = left + (right - left) * (first_width / width)
        tiles[index] = (bitmap.crop((0, 0, first_width, height)),
                        (left, bottom, split_world_x, top))
        source_parts[index][1] = len(tiles)
        tiles.append((bitmap.crop((first_width, 0, width, height)),
                      (split_world_x, bottom, right, top)))
    return tiles, source_parts


def encode_source_sprite_parts(source_parts):
    """Encode the HQP1 original-sprite-to-HKA1-tile map."""
    return (struct.pack('<4sII', b'HQP1', len(source_parts),
                        len(source_parts) + sum(tail != 0xffffffff for _, tail in source_parts)) +
            b''.join(struct.pack('<II', first, tail) for first, tail in source_parts))


def make_boss_layers(keys, sprites, clips):
    """Keep native pixels in at most four synchronized HKA1 drawing layers."""
    tiles = [sprites[key] for key in keys]
    groups = []
    for index, key in enumerate(keys):
        bitmap, (left, bottom, right, top) = sprites[key]
        width, height = bitmap.size
        if width > 1020 or height > 1020:
            raise ValueError('False Knight sprite exceeds the four-layer bound')
        nx, ny = (2 if width > 510 else 1), (2 if height > 510 else 1)
        group = []
        for y in range(ny):
            for x in range(nx):
                x0, x1 = x * width // nx, (x + 1) * width // nx
                y0, y1 = y * height // ny, (y + 1) * height // ny
                tile = (bitmap.crop((x0, y0, x1, y1)),
                        (left + (right-left)*x0/width, top - (top-bottom)*y1/height,
                         left + (right-left)*x1/width, top - (top-bottom)*y0/height))
                if not group:
                    tiles[index] = tile
                    group.append(index)
                else:
                    group.append(len(tiles))
                    tiles.append(tile)
        groups.append(group)
    blank = len(tiles)
    tiles.append((Image.new('RGBA', (1, 1)), (0., 0., 1/64, 1/64)))
    remap = {key: index for index, key in enumerate(keys)}
    result = list(clips)
    for clip in clips:
        for layer in range(1, 4):
            sources = [groups[remap[frame['native_key']]] for frame in clip['frames']]
            if not any(len(group) > layer for group in sources):
                continue
            part = copy.deepcopy(clip)
            part['name'] += f'/Part{layer}'
            if len(part['name'].encode()) >= 48:
                raise ValueError('Layer clip name exceeds HKA1 name bound')
            part['frames'] = [dict(tile_index=group[layer] if len(group)>layer else blank)
                              for group in sources]
            result.append(part)
    return tiles, result


def pack_boss_tiles(tile_sprites, page_limit, page_sizes=None):
    """Pack source pixels and geometry within an explicit scene allocation."""
    keyed_sprites = [(index, bitmap, geometry)
                     for index, (bitmap, geometry) in enumerate(tile_sprites)]
    sizes = page_sizes if page_sizes is not None else [(1024, 1024)] * page_limit
    if len(sizes) != page_limit:
        raise ValueError('Boss page sizes must match the explicit page limit')
    packed, pages = pack_pages(keyed_sprites, sizes)
    records = [packed[index] for index in range(len(tile_sprites))]
    used_pages = max((record[0] for record in records), default=-1) + 1
    return records, pages[:used_pages]


def repack_false_knight_snapshot(snapshot, manifest=None):
    """Apply the same lossless atlas budget to previously extracted artwork."""
    original = (snapshot / 'false-knight-art.bin').read_bytes()
    magic, page_count, sprite_count, clip_count, frame_count = struct.unpack_from('<4sIIII', original)
    if (magic != b'HKA1' or not 0 < page_count <= 32 or not 0 < sprite_count <= 8192
            or (manifest is not None and page_count != manifest['pages'])
            or len(original) < 20 + sprite_count * 36 + clip_count * 68 + frame_count * 4):
        raise ValueError('Invalid False Knight snapshot bank')
    source_pages = []
    for index in range(page_count):
        with Image.open(snapshot / f'false-knight-page-{index}.png') as image:
            source_pages.append(image.convert('RGBA'))
    tiles = []
    for index in range(sprite_count):
        page, x, y, width, height, *geometry = struct.unpack_from('<5I4f', original, 20 + index * 36)
        if (page >= page_count or not width or not height or
                x + width > source_pages[page].width or y + height > source_pages[page].height):
            raise ValueError('Invalid False Knight snapshot sprite')
        tiles.append((source_pages[page].crop((x, y, x + width, y + height)), geometry))
    extra_clips, extra_frames = bytearray(), []
    wave_manifest = snapshot / 'wave-spurt/manifest.json'
    if wave_manifest.exists():
        rows = json.loads(wave_manifest.read_text(encoding='utf-8'))
        if len(rows) != 6:
            raise ValueError('Invalid False Knight wave snapshot')
        first = frame_count
        for row in rows:
            with Image.open(snapshot / row['file']) as image:
                bitmap = image.convert('RGBA')
            geometry = row['geometry']
            if len(geometry) != 4:
                raise ValueError('Invalid False Knight wave geometry')
            extra_frames.append(len(tiles));tiles.append((bitmap, geometry))
        extra_clips += struct.pack('<48sfIIII', b'Wave/Spurt', 20., 2, 0, first, len(extra_frames))
    records, pages = pack_false_knight_tiles(tiles)
    sprite_bytes = b''.join(struct.pack('<5I4f', *record) for record in records)
    old_clips = original[20 + sprite_count * 36:20 + sprite_count * 36 + clip_count * 68]
    old_frames = original[20 + sprite_count * 36 + clip_count * 68:
                          20 + sprite_count * 36 + clip_count * 68 + frame_count * 4]
    trailer = original[20 + sprite_count * 36 + clip_count * 68 + frame_count * 4:]
    bank = (struct.pack('<4sIIII', b'HKA1', len(pages), len(tiles), clip_count + bool(extra_clips),
                        frame_count + len(extra_frames)) + sprite_bytes + old_clips + extra_clips +
            old_frames + struct.pack(f'<{len(extra_frames)}I', *extra_frames) + trailer)
    return bank, pages


def pack_false_knight_tiles(tile_sprites):
    """Pack full-resolution False Knight art losslessly within its 20 MiB cap."""
    keyed_sprites = [(index, bitmap, geometry)
                     for index, (bitmap, geometry) in enumerate(tile_sprites)]
    packed, pages = pack_pages(keyed_sprites, [(1024, 1024)] * 10)
    records = [packed[index] for index in range(len(tile_sprites))]
    used_pages = max((record[0] for record in records), default=-1) + 1
    return records, pages[:used_pages]


def convert(args, aliases=ALIASES, stem="boss", component_scenes=None, split_oversized=False, page_limit=0, static_clips=(), page_sizes=None, sprite_tints=None, prune_stale=True,
            complete_library=True, unity_sprites=()):
    false_knight_only = getattr(args, 'false_knight_only', False)
    if false_knight_only:
        aliases = {name: ALIASES[name] for name in ('Knight', 'Head')}
        stem = 'false-knight'
    if args.snapshot:
        output = args.output.absolute()
        output.mkdir(parents=True, exist_ok=True)
        romfs = output / 'romfs'
        romfs.mkdir(exist_ok=True)
        manifest = json.loads((args.snapshot / 'manifest.json').read_text(encoding='utf-8'))
        if false_knight_only:
            bank, pages = repack_false_knight_snapshot(args.snapshot, manifest)
            for stale in list(output.glob(f'{stem}-page-*.png')) + list(romfs.glob(f'{stem}-page-*.t3x')):
                stale.unlink()
            (romfs / f'{stem}-art.bin').write_bytes(bank)
            manifest.update(pages=len(pages), texture_bytes=len(pages) * 1024 * 1024 * 2,
                            texture_format='rgba4')
        else:
            pages = None
            shutil.copy2(args.snapshot / f'{stem}-art.bin', romfs / f'{stem}-art.bin')
        parts_path = args.snapshot / f'{stem}-parts.bin'
        if parts_path.exists():
            shutil.copy2(parts_path, romfs / parts_path.name)
        else:
            (romfs / parts_path.name).unlink(missing_ok=True)
        for index in range(manifest['pages']):
            source_png = args.snapshot / f'{stem}-page-{index}.png'
            if pages is not None:
                source_png = output / f'{stem}-page-{index}.png'
                pages[index].save(source_png)
            tex3ds_cache.run([str(args.tex3ds), '-f', 'rgba4', '-z', 'lz11', '-o',
                            str(romfs / f'{stem}-page-{index}.t3x'), str(source_png)], check=True, capture_output=True)
        if false_knight_only and getattr(args, 'effects', None):
            from false_knight_effects import append_effects
            manifest['effect_texture_bytes'] = append_effects(romfs, args.effects, args.tex3ds)
        (output / 'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
        print(json.dumps({k: manifest[k] for k in ('pages', 'sprites', 'texture_bytes')}))
        return
    if args.packages:
        sys.path.insert(0, str(args.packages))
    import UnityPy
    from UnityPy.helpers.TypeTreeGenerator import TypeTreeGenerator
    source = args.source.resolve()
    output = args.output.absolute()
    if not converted_files.private_output(output, ROOT):
        raise ValueError('Original resources must remain private')
    output.mkdir(parents=True, exist_ok=True)
    env = UnityPy.Environment(path=str(source))
    generator = TypeTreeGenerator('6000.0.61f1')
    generator.load_local_dll_folder(str(source / 'Managed'))
    env.typetree_generator = generator
    files, hashes = {}, {}

    def load(name):
        if name not in files:
            path = source / name
            hashes[name] = hashlib.sha256(path.read_bytes()).hexdigest()
            env.load_file(str(path))
            files[name] = next(f for f in env.files.values() if getattr(f, 'name', None) == name)
        return files[name]

    def resolve(owner, ref):
        name = owner.name if ref['m_FileID'] == 0 else Path(owner.externals[ref['m_FileID'] - 1].path).name
        return load(name).objects[ref['m_PathID']]

    components = []
    for scene, names in (component_scenes if component_scenes is not None else [('level40', {'Giant Fly', 'Fly', 'Fly Spawn'}),
                         ('level48', {'False Knight New', 'Head', 'Death Head'})]):
        scene_file = load(scene)
        for obj in list(scene_file.objects.values()):
            if obj.type.name != 'GameObject':
                continue
            actor = obj.read_typetree()
            if actor['m_Name'] not in names:
                continue
            for entry in actor['m_Component']:
                component = scene_file.objects[entry['component']['m_PathID']]
                record = dict(scene=scene, actor=actor['m_Name'], path_id=component.path_id,
                              native_type=component.type.name)
                try:
                    record['data'] = component.read_typetree()
                except (ValueError, EOFError, IndexError) as error:
                    record['unsupported'] = type(error).__name__
                components.append(record)
                if stem != 'boss' and component.type.name == 'Transform':
                    pending = [(ref, 0., 0.) for ref in record['data']['m_Children']]
                    while pending:
                        ref, px, py = pending.pop()
                        child = resolve(scene_file, ref)
                        transform = child.read_typetree()
                        x = px + transform['m_LocalPosition']['x']
                        y = py + transform['m_LocalPosition']['y']
                        go = resolve(child.assets_file, transform['m_GameObject']).read_typetree()
                        for entry in go['m_Component']:
                            collider = resolve(child.assets_file, entry['component'])
                            if collider.type.name == 'BoxCollider2D':
                                components.append(dict(scene=scene, actor=actor['m_Name'],
                                    path_id=collider.path_id, native_type=collider.type.name,
                                    local_position=[x,y], data=collider.read_typetree()))
                        pending.extend((ref,x,y) for ref in transform['m_Children'])
    (output / 'source-components.json').write_text(json.dumps(components, indent=2), encoding='utf-8')

    clips, source_clips, sprites, atlas_cache = [], {}, {}, {}
    has_knight = 'Knight' in aliases
    collections, materials = {}, {}
    for prefix, (name, library_id, aliases) in aliases.items():
        print(f'{stem}: decoding {prefix} clips', flush=True)
        library = load(name).objects[library_id].read_typetree()
        source_clips[prefix] = library['clips']
        available = {c['name']: c for c in library['clips'] if c['name']}
        # The shipped False Knight bank uses these explicit animation aliases.
        # Expanding the whole library exceeds the original ten-page allocation.
        # Fixed banks (False Knight, Uumuu body and Quirrel) keep exactly the
        # clips their runtime tables name; complete_library=False selects that.
        clip_aliases = (aliases if false_knight_only or not complete_library
                        else complete_aliases(library['clips'], aliases))
        for alias, original in clip_aliases.items():
            if original not in available:
                raise ValueError(f'{prefix} missing source clip {original}; found {list(available)}')
            clip = copy.deepcopy(available[original])
            clip['name'] = prefix + '/' + alias
            if false_knight_only and clip['name'] in ('Knight/Corpse', 'Head/Death'):
                # The shipped runtime holds the first authored corpse pose.
                clip.update(frames=clip['frames'][:1], wrapMode=1, loopStart=0)
            # Source FSM holds belong in playback, never in the library data.
            for frame in clip['frames']:
                collection_obj = resolve(load(name), frame['spriteCollection'])
                key = (collection_obj.assets_file.name, collection_obj.path_id, frame['spriteId'])
                frame['native_key'] = key
                if key in sprites:
                    continue
                collection_key = key[:2]
                if collection_key not in collections:
                    collections[collection_key] = collection_obj.read_typetree()
                collection = collections[collection_key]
                definition = collection['spriteDefinitions'][frame['spriteId']]
                material_id = definition['materialId']
                material_obj = resolve(collection_obj.assets_file, collection['materials'][material_id])
                material_key = (material_obj.assets_file.name, material_obj.path_id)
                if material_key not in materials:
                    materials[material_key] = material_obj.read_typetree()
                material = materials[material_key]
                textures = dict(material['m_SavedProperties']['m_TexEnvs'])
                texture_obj = resolve(material_obj.assets_file, textures['_MainTex']['m_Texture'])
                texture_key = (texture_obj.assets_file.name, texture_obj.path_id)
                if texture_key not in atlas_cache:
                    atlas_cache[texture_key] = texture_obj.read().image.convert('RGBA')
                normalized = dict(definition, materialId=0)
                bitmap, geometry = decode_sprite(atlas_cache[texture_key], normalized)
                bitmap = bitmap.resize((max(1, round(bitmap.width * args.scale)),
                                        max(1, round(bitmap.height * args.scale))), Image.Resampling.LANCZOS)
                if sprite_tints and prefix in sprite_tints:
                    channels = list(bitmap.split())
                    for channel, gain in enumerate(sprite_tints[prefix]):
                        channels[channel] = channels[channel].point(lambda value: round(value * gain))
                    bitmap = Image.merge('RGBA', channels)
                sprites[key] = bitmap, geometry
            clips.append(clip)
    if has_knight:
        # Falling Barrel is a native Unity Sprite, not part of the tk2d library.
        asset = load('sharedassets48.assets')
        barrel = asset.objects[51].read()
        bitmap = barrel.image.convert('RGBA')
        geometry = unity_sprite_geometry(barrel, bitmap.size)
        bitmap = bitmap.resize((max(1, round(bitmap.width * args.scale)),
                                max(1, round(bitmap.height * args.scale))), Image.Resampling.LANCZOS)
        key = (asset.name, 51, 0)
        sprites[key] = bitmap, geometry
        clips.append(dict(name='Barrel/Idle', fps=1., wrapMode=1, loopStart=0,
                          frames=[dict(native_key=key)]))
        # FalseyControl spawns sharedassets32's authored six-frame Shockwave
        # Spurt along each travelling ground wave.
        wave_asset = load('sharedassets32.assets')
        wave_library = wave_asset.objects[745].read_typetree()
        wave_clip = next((clip for clip in wave_library['clips']
                          if clip['name'] == 'Shockwave Spurt'), None)
        if not wave_clip or len(wave_clip['frames']) != 6:
            raise ValueError('Unexpected False Knight shockwave art')
        wave_frames = []
        for frame in wave_clip['frames']:
            collection_obj = resolve(wave_asset, frame['spriteCollection'])
            wave_key = (collection_obj.assets_file.name, collection_obj.path_id, frame['spriteId'])
            if wave_key not in sprites:
                collection = collection_obj.read_typetree()
                definition = collection['spriteDefinitions'][frame['spriteId']]
                material_obj = resolve(collection_obj.assets_file, collection['materials'][definition['materialId']])
                material = material_obj.read_typetree()
                texture_obj = resolve(material_obj.assets_file,
                                      dict(material['m_SavedProperties']['m_TexEnvs'])['_MainTex']['m_Texture'])
                texture_key = (texture_obj.assets_file.name, texture_obj.path_id)
                if texture_key not in atlas_cache:
                    atlas_cache[texture_key] = texture_obj.read().image.convert('RGBA')
                wave_bitmap, wave_geometry = decode_sprite(atlas_cache[texture_key], dict(definition, materialId=0))
                wave_bitmap = wave_bitmap.resize((max(1, round(wave_bitmap.width * args.scale)),
                                                  max(1, round(wave_bitmap.height * args.scale))), Image.Resampling.LANCZOS)
                sprites[wave_key] = wave_bitmap, wave_geometry
            wave_frames.append(dict(native_key=wave_key))
        wave = copy.deepcopy(wave_clip);wave['name']='Wave/Spurt';wave['frames']=wave_frames
        clips.append(wave)
    # Native Unity Sprites (SpriteRenderer projectiles) become one-frame clips: (clip, assets, path id).
    for clip_name, assets_name, sprite_id in unity_sprites:
        asset = load(assets_name)
        sprite = asset.objects[sprite_id].read()
        bitmap = sprite.image.convert('RGBA')
        geometry = unity_sprite_geometry(sprite, bitmap.size)
        bitmap = bitmap.resize((max(1, round(bitmap.width * args.scale)),
                                max(1, round(bitmap.height * args.scale))), Image.Resampling.LANCZOS)
        key = (asset.name, sprite_id, 0)
        sprites[key] = bitmap, geometry
        clips.append(dict(name=clip_name, fps=1., wrapMode=1, loopStart=0, frames=[dict(native_key=key)]))
    for index, (name, bitmap, geometry) in enumerate(static_clips):
        key = ('static-title', 0, index)
        sprites[key] = bitmap, geometry
        clips.append(dict(name=name, fps=1, wrapMode=1, loopStart=0,
                          frames=[dict(native_key=key)]))
    keys = sorted(sprites)
    if false_knight_only:
        tile_sprites, clips = make_boss_layers(keys, sprites, clips)
        source_sprite_parts = []
    else:
        tile_sprites, source_sprite_parts = make_sprite_tiles(keys, sprites, split_oversized)
    if false_knight_only:
        tile_sprites = [trim_transparent(bitmap, geometry) for bitmap, geometry in tile_sprites]
        records, pages = pack_false_knight_tiles(tile_sprites)
    elif page_limit:
        records, pages = pack_boss_tiles(tile_sprites, page_limit, page_sizes)
    else:
        pages = [Image.new('RGBA', (1024, 1024))]
        records, x, y, row = [None] * len(tile_sprites), 0, 0, 0
        for tile_index in sorted(range(len(tile_sprites)),
                                 key=lambda i: (-tile_sprites[i][0].height, -tile_sprites[i][0].width, i)):
            bitmap, geometry = tile_sprites[tile_index]
            w, h = bitmap.size
            if max(w, h) + 2 > 1024:
                raise ValueError(f'Tile exceeds 1024 atlas: {tile_index} ({w}x{h})')
            if x + w + 2 > 1024:
                x, y, row = 0, y + row, 0
            if y + h + 2 > 1024:
                pages.append(Image.new('RGBA', (1024, 1024)))
                x = y = row = 0
            pages[-1].paste(bitmap, (x + 1, y + 1))
            records[tile_index] = (len(pages) - 1, x + 1, y + 1, w, h, *geometry)
            x += w + 2
            row = max(row, h + 2)
    remap = {key: i for i, key in enumerate(keys)}
    romfs = output / 'romfs'
    romfs.mkdir(exist_ok=True)
    for stale in (list(output.glob(f'{stem}-page-*.png')) + list(romfs.glob(f'{stem}-page-*.t3x')) if prune_stale else []):
        stale.unlink()
    if not split_oversized and prune_stale:
        stale_parts = romfs / f'{stem}-parts.bin'
        if stale_parts.exists():
            stale_parts.unlink()
    with (romfs / f'{stem}-art.bin').open('wb') as stream:
        stream.write(struct.pack('<4sIIII', b'HKA1', len(pages), len(tile_sprites), len(clips), sum(len(c['frames']) for c in clips)))
        for record in records:
            stream.write(struct.pack('<5I4f', *record))
        offset = 0
        for clip in clips:
            stream.write(struct.pack('<48sfIIII', clip['name'].encode(), clip['fps'], clip['wrapMode'], clip['loopStart'], offset, len(clip['frames'])))
            offset += len(clip['frames'])
        for clip in clips:
            for frame in clip['frames']:
                stream.write(struct.pack('<I', frame['tile_index'] if 'tile_index' in frame
                                         else remap[frame['native_key']]))
        stream.write(event_bytes([(c['name'],c,c['frames']) for c in clips]))
    if split_oversized:
        parts_path = romfs / f'{stem}-parts.bin'
        parts_path.write_bytes(encode_source_sprite_parts(source_sprite_parts))
    for i, page in enumerate(pages):
        png = output / f'{stem}-page-{i}.png'
        page.save(png)
        tex3ds_cache.run([str(args.tex3ds), '-f', 'rgba4', '-z', 'lz11', '-o', str(romfs / f'{stem}-page-{i}.t3x'), str(png)], check=True, capture_output=True)
    limitations = ['FSM sequencing and frame events are not implemented by this converter.']
    if not false_knight_only:
        limitations[:0] = ['Presentation aliases include approximate Fall/Slam/Death choices.',
                           'Gruzzer Death reuses Fly; original death effects are separate.',
                           'Artwork downsampled to half size for New 3DS texture RAM; world geometry remains original.']
    manifest = dict(build_id=22529139, source_sha256=hashes, aliases=aliases,
                    source_clips=source_clips, pages=len(pages), sprites=len(tile_sprites),
                    texture_bytes=sum(page.width*page.height*2 for page in pages), bitmap_scale=args.scale,
                    limitations=limitations)
    if split_oversized:
        manifest['source_sprite_parts'] = {str(index): [first, None if tail == 0xffffffff else tail]
                                           for index, (first, tail) in enumerate(source_sprite_parts)}
        manifest['source_sprite_count'] = len(source_sprite_parts)
    if args.save_snapshot:
        args.save_snapshot.mkdir(parents=True, exist_ok=True)
        shutil.copy2(romfs / f'{stem}-art.bin', args.save_snapshot / f'{stem}-art.bin')
        if split_oversized:
            shutil.copy2(romfs / f'{stem}-parts.bin', args.save_snapshot / f'{stem}-parts.bin')
        (args.save_snapshot / 'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
        for index in range(len(pages)):
            shutil.copy2(output / f'{stem}-page-{index}.png', args.save_snapshot / f'{stem}-page-{index}.png')
    if false_knight_only and getattr(args, 'effects', None):
        from false_knight_effects import append_effects
        manifest['effect_texture_bytes'] = append_effects(romfs, args.effects, args.tex3ds)
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    print(json.dumps({k: manifest[k] for k in ('pages', 'sprites', 'texture_bytes')}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / '_private/original/Hollow Knight/hollow_knight_Data')
    parser.add_argument('--snapshot', type=Path, help='Convert an already extracted private snapshot without UnityPy')
    parser.add_argument('--save-snapshot', type=Path, help='Save the source art snapshot after explicit extraction')
    parser.add_argument('--effects', type=Path, help='Copied False Knight particle instance/texture export')
    parser.add_argument('--output', type=Path, default=ROOT / '_private/converted/crossroads-bosses')
    parser.add_argument('--packages', type=Path, default=ROOT / '_private/tools/boss-python')
    parser.add_argument('--tex3ds', type=Path, default=Path(TEX3DS_EXE))
    parser.add_argument('--scale', type=float, default=.5)
    parser.add_argument('--false-knight-only', action='store_true',
                        help='Build a separate full-resolution Knight + Head bank')
    parser.add_argument('--named-clips-only', action='store_true',
                        help='Snapshot only the aliased clips (the PSP transcoder completes libraries)')
    args = parser.parse_args()
    convert(args, complete_library=not args.named_clips_only)
