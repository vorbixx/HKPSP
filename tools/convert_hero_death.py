"""Add the original normal-death particle and nail artwork to the hero atlas."""
import converted_files  # Install hardlink copy-on-write before asset writes.
import hashlib
import json
from PIL import Image
from convert_room_visuals import decode


def death_sprites(source):
    mapping = json.loads((source / 'death-map.json').read_text())
    for name, digest in mapping['source_sha256'].items():
        if hashlib.sha256((source / name).read_bytes()).hexdigest() != digest:
            raise ValueError('Changed hero death source: ' + name)
    nodes = {n['name']: n for n in mapping['nodes']}
    sprites, clips = [], []
    for node, name in (('Particle Wave', 'Death Mote'), ('Ash L', 'Death Ash'), ('Corpse Nail Hero', 'Death Nail')):
        n = nodes[node]
        image = Image.open(source / n['image']).convert('RGBA')
        if 'sprite' in n:
            image, bounds = decode(image, json.loads((source / n['sprite']).read_text()))
        else:
            # Unity particle startSize is the quad's world width/height.
            bounds = (-.5, -.5, .5, .5)
        # Ash uses one of three authored rows (single-row/random-row UV module).
        tiles = 3 if name == 'Death Ash' else 1
        for row in range(tiles):
            bitmap = image.crop((0, row * image.height // tiles, image.width,
                                 (row + 1) * image.height // tiles)) if tiles > 1 else image
            i = len(sprites)
            sprites.append(((-1, i), bitmap, bounds))
            clips.append(dict(name=name + str(row) if tiles > 1 else name, fps=1, wrapMode=6, loopStart=0,
                              frames=[dict(spriteCollection={'m_PathID': -1}, spriteId=i)]))
    for flash in mapping['flashes']:
        frames = []
        fps = 12 if flash['name'] == 'Death Flash' else 20
        for index, frame in enumerate(flash['frames']):
            if abs(frame['time']-index/fps) > 1e-6:
                raise ValueError('Nonuniform native death flash frames')
            bitmap, bounds = decode(Image.open(source/frame['image']).convert('RGBA'),
                                    json.loads((source/frame['sprite']).read_text()))
            i = len(sprites)
            sprites.append(((-1, i), bitmap, bounds))
            frames.append(dict(spriteCollection={'m_PathID': -1}, spriteId=i))
        clips.append(dict(name=flash['name'], fps=fps, wrapMode=2, loopStart=0, frames=frames))
    inputs = [source / name for name in mapping['source_sha256']]
    return sprites, clips, inputs
