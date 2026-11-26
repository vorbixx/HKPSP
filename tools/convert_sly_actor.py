"""Compile bounded original Sly/toll tk2d clips using the existing animation-bank format."""
import converted_files
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
from PIL import Image
from convert_hero import decode_sprite, trim_transparent
from watcher_atlas import place_rectangles
from tool_paths import TEX3DS as TEX3DS_EXE

ROOT = Path(__file__).resolve().parents[1]

def compile_actor(target, *, level=17, actor_id=13, animator_id=415,
                  actor_name='Sly Shop', stem='sly-shop-actor', clip_names=('Idle',), atlas_size=512, fit_atlas=False):
    dependencies = os.environ.get('HK3DS_UNITYPY_PATH')
    if dependencies:
        sys.path.insert(0, dependencies)
    sys.path.append(str(ROOT / '_private/tools/boss-python'))
    import UnityPy
    from watcher_unity import AlignedGenerator
    source = ROOT / '_private/original/Hollow Knight/hollow_knight_Data'
    env = UnityPy.Environment(path=str(source))
    generator = AlignedGenerator('6000.0.61f1')
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
        name = owner.name if not ref['m_FileID'] else Path(owner.externals[ref['m_FileID']-1].path).name
        return load(name).objects[ref['m_PathID']]
    scene = load(f'level{level}')
    actor = scene.objects[actor_id].read_typetree()
    if actor['m_Name'] != actor_name:
        raise ValueError('Unreviewed tk2d actor')
    parts = [resolve(scene, c['component']) for c in actor['m_Component']]
    transform = next(p.read_typetree() for p in parts if p.type.name == 'Transform')
    animator = scene.objects[animator_id].read_typetree()
    library = resolve(scene, animator['library'])
    source_clips = library.read_typetree()['clips']
    clips = [next(c for c in source_clips if c['name'] == name) for name in clip_names]
    if source_clips[animator['defaultClipId']]['name'] != clip_names[0]:
        raise ValueError('Original actor default animation changed')
    sprites, collections, textures = {}, {}, {}
    frame_keys = []
    for frame in (f for clip in clips for f in clip['frames']):
        bank = resolve(library.assets_file, frame['spriteCollection'])
        key = (bank.assets_file.name, bank.path_id, frame['spriteId'])
        frame_keys.append(key)
        if key in sprites:
            continue
        bank_key = key[:2]
        if bank_key not in collections:
            collections[bank_key] = bank.read_typetree()
            texture = resolve(bank.assets_file, collections[bank_key]['textures'][0])
            textures[bank_key] = texture.read().image.convert('RGBA')
        definition = collections[bank_key]['spriteDefinitions'][frame['spriteId']]
        if definition['materialId'] != 0:
            raise ValueError('Unsupported tk2d actor material')
        sprites[key] = trim_transparent(*decode_sprite(textures[bank_key], definition))
    keys = sorted(sprites)
    original_sprites = sprites
    pixel_scale = 1.0
    while True:
        placements, pages = place_rectangles([sprites[k][0].size for k in keys], atlas_size)
        if pages == 1:
            break
        if not fit_atlas or pixel_scale <= .25:
            raise ValueError('Actor exceeds one bounded atlas')
        pixel_scale *= .5
        # Keep all source frames and world-space bounds; reduce texture sampling for the 3DS screen.
        sprites = {key: (bitmap.resize(tuple(max(1, round(v * pixel_scale)) for v in bitmap.size),
                                       Image.Resampling.LANCZOS), bounds)
                   for key, (bitmap, bounds) in original_sprites.items()}
    extent = [max(placements[i][d+1]+sprites[k][0].size[d]+1 for i,k in enumerate(keys)) for d in (0,1)]
    size = tuple(max(8, 1 << (v-1).bit_length()) for v in extent)
    image = Image.new('RGBA', size)
    binary = bytearray(struct.pack('<4s4I', b'HKA1', 1, len(keys), len(clips), len(frame_keys)))
    for i,key in enumerate(keys):
        bitmap, bounds = sprites[key]
        page,x,y = placements[i]
        image.paste(bitmap, (x,y))
        binary += struct.pack('<5I4f', page,x,y,*bitmap.size,*bounds)
    first = 0
    for clip in clips:
        name = 'Sly Idle' if stem == 'sly-shop-actor' else clip['name']
        binary += struct.pack('<48sf4I', name.encode(), clip['fps'], clip['wrapMode'], clip['loopStart'],first,len(clip['frames']))
        first += len(clip['frames'])
    for key in frame_keys:
        binary += struct.pack('<I', keys.index(key))
    target.mkdir(parents=True, exist_ok=True)
    png = target / (stem + '-page-0.png')
    image.save(png)
    romfs = target.parent / 'hero/romfs'
    (romfs / (stem + '-art.bin')).write_bytes(binary)
    subprocess.run([TEX3DS_EXE,'-f','rgba4','-z','lz11','-o',str(romfs/(stem+'-page-0.t3x')),str(png)],check=True,capture_output=True)
    metadata = dict(owner=actor_id, animator=animator_id, clips=list(clip_names), transform=transform, source_sha256=hashes,
                    texture_bytes=size[0]*size[1]*2, pixel_scale=pixel_scale, sprites=len(keys), frames=len(frame_keys))
    (target / (stem + '-source.json')).write_text(json.dumps(metadata,indent=2))
    return transform['m_LocalPosition']
