"""Convert a private AssetRipper tk2d export to a native-resolution 3DS pack.

UVs (including rotated frames), geometry, frame order, fps, loop sections and
event payloads are retained. Only rectangular single-material sprites are
accepted. PNGs/t3x/metadata must remain under _private or build.
"""
import converted_files  # Install hardlink copy-on-write before asset writes.
import argparse
import hashlib
import json
import math
import struct
import tex3ds_cache
import shutil
from pathlib import Path
from PIL import Image
from convert_nail import convert as convert_nail
from private_output_paths import is_private_output
from tool_paths import TEX3DS as TEX3DS_EXE

ROOT = Path(__file__).resolve().parents[1]
CORE = ['Idle', 'Enter', 'Exit Door To Idle', 'Sit', 'Sit Idle', 'Get Off', 'Idle Hurt', 'Run', 'Sprint', 'Walk', 'Airborne', 'Land', 'Run To Idle',
        'Turn', 'TurnToBG', 'TurnFromBG', 'Dash', 'Dash To Idle', 'Dash Down', 'Shadow Dash', 'Shadow Dash Sharp',
        'Shadow Dash Down', 'Shadow Dash Down Sharp', 'Double Jump', 'Walljump', 'Wall Slide',
        'Slash', 'SlashAlt', 'UpSlash', 'DownSlash', 'Wall Slash', 'HardLand', 'Recoil', 'Stun',
        'Focus', 'Focus End', 'Focus Get', 'Focus Get Once', 'LookUp', 'LookDown', 'LookUpEnd', 'LookDownEnd',
        'SlashEffect', 'SlashEffectAlt', 'UpSlashEffect', 'DownSlashEffect',
        'SlashEffect M', 'SlashEffectAlt M', 'UpSlashEffect M', 'DownSlashEffect M',
        'Collect Normal 1', 'Collect Normal 2', 'Super Hard Land', 'Collect Normal 3', 'Wake Up Ground', 'Hazard Respawn',
        'Death', 'Death Head Normal', 'Death Head Cracked',
        'Surface In', 'Surface InToIdle', 'Surface InToSwim', 'Surface Idle', 'Surface Swim',
        'Fireball1 Cast', 'Fireball2 Cast']


def decode_sprite(atlas, sprite):
    if sprite['complexGeometry'] or len(sprite['positions']) != 4 or len(sprite['uvs']) != 4:
        raise ValueError('Unsupported non-rectangular geometry: ' + sprite['name'])
    if sprite['materialId'] != 0:
        raise ValueError('Multiple source textures need an explicit texture map')
    positions, uv = sprite['positions'], sprite['uvs']
    left, right = min(p['x'] for p in positions), max(p['x'] for p in positions)
    bottom, top = min(p['y'] for p in positions), max(p['y'] for p in positions)
    # Match vertices by position, not an assumed winding or flipped enum value.
    def texcoord(x, y):
        i = next(i for i,p in enumerate(positions) if p['x'] == x and p['y'] == y)
        return round(uv[i]['x'] * atlas.width), round((1 - uv[i]['y']) * atlas.height)
    tl, tr, bl = texcoord(left, top), texcoord(right, top), texcoord(left, bottom)
    width = round(math.dist(tl, tr)); height = round(math.dist(tl, bl))
    if width <= 0 or height <= 0:
        raise ValueError('Empty sprite: ' + sprite['name'])
    matrix = ((tr[0]-tl[0])/width, (bl[0]-tl[0])/height, tl[0],
              (tr[1]-tl[1])/width, (bl[1]-tl[1])/height, tl[1])
    result = atlas.transform((width, height), Image.Transform.AFFINE, matrix, resample=Image.Resampling.NEAREST)
    return result, (left, bottom, right, top)


def trim_transparent(bitmap, bounds):
    """Trim alpha-empty margins and move the authored bounds by the same scale."""
    box = bitmap.getchannel('A').getbbox()
    if box is None:
        return bitmap, bounds
    x0, y0, x1, y1 = (max(0, box[0] - 1), max(0, box[1] - 1),
                      min(bitmap.width, box[2] + 1), min(bitmap.height, box[3] + 1))
    left, bottom, right, top = bounds
    dx, dy = (right - left) / bitmap.width, (top - bottom) / bitmap.height
    return bitmap.crop((x0, y0, x1, y1)), (
        left + x0 * dx, top - y1 * dy, left + x1 * dx, top - y0 * dy)


def deduplicate_bitmaps(sprites):
    """Return one stored copy per exact RGBA bitmap and an ID-to-ID remap."""
    unique, remap, candidates = [], {}, {}
    for sprite_id, bitmap, geometry in sprites:
        digest = (bitmap.size, hashlib.sha256(bitmap.tobytes()).digest())
        canonical = next((key for key, pixels in candidates.get(digest, ())
                          if pixels == bitmap.tobytes()), None)
        if canonical is None:
            canonical = sprite_id
            unique.append((sprite_id, bitmap, geometry))
            candidates.setdefault(digest, []).append((sprite_id, bitmap.tobytes()))
        remap[sprite_id] = canonical
    return unique, remap


def pack_fixed_hero_pages(sprites, page_size=1024, page_count=4):
    """Trim transparent margins, share exact bitmaps, then pack within the fixed page budget."""
    from atlas_packing import pack_pages

    trimmed = [(key, *trim_transparent(bitmap, geometry)) for key, bitmap, geometry in sprites]
    unique, remap = deduplicate_bitmaps(trimmed)
    records, pages = pack_pages(unique, [(page_size, page_size)] * page_count)
    padded_pages = []
    for page in pages:
        if page.width > page_size or page.height > page_size:
            raise ValueError('Packed hero page exceeds the fixed page size')
        if page.size != (page_size, page_size):
            padded = Image.new('RGBA', (page_size, page_size))
            padded.paste(page, (0, 0))
            page = padded
        padded_pages.append(page)
    pages = padded_pages
    for sprite_id, bitmap, geometry in trimmed:
        canonical = records[remap[sprite_id]]
        records[sprite_id] = (*canonical[:5], *geometry)
    return records, pages


def convert(args):
    output = args.output.absolute()
    if not converted_files.private_output(output, ROOT, allow_build=True):
        raise ValueError('Output must stay inside _private or build')
    output.mkdir(parents=True, exist_ok=True)
    source = args.assets
    library_file = source / 'c1-20600.json'
    library = json.loads(library_file.read_text())['m_Structure']['clips']
    by_name = {c['name']: c for c in library if c['name']}
    names = sorted(by_name) if args.all else CORE
    missing = [n for n in names if n not in by_name]
    if missing: raise ValueError('Missing clips: ' + ', '.join(missing))
    clips = [by_name[n] for n in names]
    sprite_ids = sorted({(f['spriteCollection']['m_PathID'], f['spriteId']) for c in clips for f in c['frames']})
    for c in clips:
        if c['wrapMode'] not in (0, 1, 2, 3, 6): raise ValueError('Unsupported random playback: ' + c['name'])
        if any(f['spriteCollection']['m_FileID'] != 0 for f in c['frames']):
            raise ValueError('External sprite collection: ' + c['name'])
    collections, atlases = {}, {}
    inputs = [library_file]
    for collection_id in sorted({key[0] for key in sprite_ids}):
        path = source / f'c1-{collection_id}.json'
        collection = json.loads(path.read_text())['m_Structure']
        if len(collection['textures']) != 1 or collection['textures'][0]['m_FileID'] != 0:
            raise ValueError('Unsupported texture map')
        texture_file = source / f"texture-{collection['textures'][0]['m_PathID']}.png"
        collections[collection_id] = collection
        atlases[collection_id] = Image.open(texture_file).convert('RGBA')
        inputs.extend([path,texture_file])
    sprites = [(key, *decode_sprite(atlases[key[0]], collections[key[0]]['spriteDefinitions'][key[1]])) for key in sprite_ids]
    from convert_hero_death import death_sprites
    extras, extra_clips, extra_inputs = death_sprites(source.parent / 'hero-death')
    from convert_hero_effects import compile_effects
    fx_sprites, fx_clips, fx_inputs = compile_effects(source.parent / 'hero-effects', output)
    extras.extend(fx_sprites)
    extra_clips.extend(fx_clips)
    extra_inputs.extend(fx_inputs)
    from convert_focus_effects import compile_focus
    focus_sprites, focus_clips, focus_inputs = compile_focus(source.parent / 'focus-effects', output)
    extras.extend(focus_sprites)
    extra_clips.extend(focus_clips)
    extra_inputs.extend(focus_inputs)
    sprites.extend(extras)
    sprites = [(key, *trim_transparent(bitmap, geometry)) for key, bitmap, geometry in sprites]
    sprite_ids.extend(s[0] for s in extras)
    clips.extend(extra_clips)
    inputs.extend(extra_inputs)
    sprites.sort(key=lambda s: (-s[1].height, -s[1].width, s[0]))
    page_size = 1024
    pages = [Image.new('RGBA', (page_size, page_size))]
    x = y = row = 0
    records = {}
    for sprite_id, bitmap, geometry in sprites:
        width, height = bitmap.size
        if width + 2 > page_size or height + 2 > page_size: raise ValueError('Sprite exceeds 1024 page')
        if x + width + 2 > page_size: x = 0; y += row; row = 0
        if y + height + 2 > page_size: pages.append(Image.new('RGBA', (page_size, page_size))); x = y = row = 0
        pages[-1].paste(bitmap, (x+1,y+1))
        records[sprite_id] = (len(pages)-1,x+1,y+1,width,height,*geometry)
        x += width + 2; row = max(row,height+2)
    if not args.all and len(pages)>4:
        # Focus and cast poses retain authored full-resolution art: one bounded RGBA4 page (2 MiB).
        # The resident package memory audit checks the resulting five-page total.
        records, pages = pack_fixed_hero_pages(sprites, page_size, 5)
    remap = {sprite_id: i for i,sprite_id in enumerate(sprite_ids)}
    total_frames = sum(len(c['frames']) for c in clips)
    with (output/'hero.bin').open('wb') as out:
        out.write(struct.pack('<4sIIII',b'HKA1',len(pages),len(sprite_ids),len(clips),total_frames))
        for sprite_id in sprite_ids: out.write(struct.pack('<5I4f',*records[sprite_id]))
        offset = 0
        for c in clips:
            encoded = c['name'].encode()
            if len(encoded) >= 48: raise ValueError('Clip name too long')
            out.write(struct.pack('<48sfIIII',encoded,c['fps'],c['wrapMode'],c['loopStart'],offset,len(c['frames'])))
            offset += len(c['frames'])
        for c in clips:
            for f in c['frames']: out.write(struct.pack('<I',remap[(f['spriteCollection']['m_PathID'],f['spriteId'])]))
    for i, page in enumerate(pages):
        png, t3x = output/f'page-{i}.png', output/f'page-{i}.t3x'
        page.save(png)
        if args.tex3ds is not None:
            tex3ds_cache.run([str(args.tex3ds),'-f','rgba4','-z','lz11','-o',str(t3x),str(png)],check=True)
    manifest = {'source_sha256': {str(p.name): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs},
                'format': 'HKA1', 'page_size': page_size, 'page_sizes': [list(p.size) for p in pages],
                'pages': len(pages), 'texture_format': 'rgba4', 'texture_bytes': sum(p.width*p.height*2 for p in pages),
                'sprites': len(sprite_ids), 'frames': total_frames,
                'clips': [{k:c[k] for k in ('name','fps','wrapMode','loopStart','frames')} for c in clips],
                'note': 'Frame event payloads preserved here; native runtime event dispatch is not yet implemented.'}
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
    romfs = output/'romfs'
    romfs.mkdir(exist_ok=True)
    shutil.copyfile(output/'hero.bin',romfs/'hero.bin')
    shutil.copyfile(output/'hero-effects.bin',romfs/'hero-effects.bin')
    shutil.copyfile(output/'focus-effects.bin',romfs/'focus-effects.bin')
    convert_nail(source,output)
    shutil.copyfile(output/'nail.bin',romfs/'nail.bin')
    # Remove only obsolete generated page files, never recurse through a user path.
    for stale in romfs.glob('page-*.t3x'):
        if stale.name not in {f'page-{i}.t3x' for i in range(len(pages))}: stale.unlink()
    if args.tex3ds is not None:
        for i in range(len(pages)): shutil.copyfile(output/f'page-{i}.t3x',romfs/f'page-{i}.t3x')
    # Small visual QA contact sheet; no original images enter the public tree.
    preview = Image.new('RGBA',(768,384),(30,34,42,255))
    for i,name in enumerate(['Idle','Run','Airborne','Dash','Slash','UpSlash','DownSlash','Wall Slide']):
        frame=by_name[name]['frames'][0]
        collection_id=frame['spriteCollection']['m_PathID']
        sprite,_=decode_sprite(atlases[collection_id],collections[collection_id]['spriteDefinitions'][frame['spriteId']])
        sprite.thumbnail((170,170))
        preview.alpha_composite(sprite,((i%4)*192+10,(i//4)*192+10))
    preview.save(output/'preview.png')
    print(json.dumps({k:manifest[k] for k in ['pages','texture_format','texture_bytes','sprites','frames']}))


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--assets',type=Path,default=ROOT/'_private/research/assets')
    parser.add_argument('--output',type=Path,default=ROOT/'_private/converted/hero')
    parser.add_argument('--tex3ds',type=Path,default=Path(TEX3DS_EXE))
    parser.add_argument('--all',action='store_true')
    convert(parser.parse_args())
