"""Admit the original, short-lived City Crest Slam tk2d animation."""
import hashlib
import json
import math
import struct

from PIL import Image
from assetripper_client import ROOT
from convert_hero import decode_sprite
from convert_room_visuals import project
from convert_city_crest import SOURCE as GATE_SOURCE
from convert_city_toll import one
from inspect_fsm import decode_fsm

SOURCE = ROOT / '_private/minimum-obstacles/city-crest-slam-art'


def compile_slam(art, source, first, canonical, require):
    manifest = json.loads((art / 'sources.json').read_text())
    require(manifest['scene'] == 'Fungus2_21' and manifest['owner'] == 97, 'Wrong City slam source')
    for name, digest in manifest['sha256'].items():
        require(hashlib.sha256((art / name).read_bytes()).hexdigest() == digest, 'Changed original City slam art')

    def read(ident):
        return json.loads((art / f'level186-{ident}.json').read_text())

    def dependency(name):
        return json.loads((art / manifest['dependencies'][name]['file']).read_text())['m_Structure']

    sprite, animator = read(5248)['m_Structure'], read(5391)['m_Structure']
    clips = dependency('library')['clips']
    require(animator['playAutomatically'] and animator['defaultClipId'] == 5 and sprite['_spriteId'] == 59,
            'Changed City slam activation')
    clip = clips[5]
    ids = [59, 60, 58, 56, 57, 55]
    require(clip['name'] == 'Slam' and clip['fps'] == 18 and clip['wrapMode'] == 2 and
            [f['spriteId'] for f in clip['frames']] == ids and all(
                not f.get('triggerEvent') and f['spriteCollection'] == dict(m_FileID=0, m_PathID=21014)
                for f in clip['frames']), 'Changed City slam animation')
    require(read(97)['m_Name'] == 'Slam Effect' and not read(97)['m_IsActive'], 'City slam must start inactive')
    fsm = decode_fsm(json.loads((GATE_SOURCE / 'level186-5613.json').read_text())['m_Structure']['fsm'])
    activation = one(fsm, 'Slammed', 'ActivateGameObject')
    require(activation['gameObject']['gameObject']['name'] == 'Slam Effect' and
            activation['activate']['value'] and not activation['everyFrame'], 'Changed City slam dispatch')
    collection = dependency('collection')
    texture = next(t for t in manifest['textures'] if t['collection_file'] == manifest['dependencies']['collection']['file'])
    atlas = Image.open(art / texture['file']).convert('RGBA')
    sprites = [(first + i, *decode_sprite(atlas, collection['spriteDefinitions'][ident]))
               for i, ident in enumerate(ids)]
    # The source frame IDs are sparse; the appended bank uses contiguous local IDs.
    compiled_clip = dict(clip, frames=[dict(spriteId=first + i) for i in range(len(ids))])
    chain, ident = [], 1813
    while ident:
        require(len(chain) < 32, 'Cyclic City slam transform')
        transform = canonical(json.loads((source / f'level186-{ident}.json').read_text()))
        chain.append(transform)
        ident = transform['m_Father']['m_PathID']
    require(any(t['m_GameObject']['m_PathID'] == 1466 for t in chain), 'City slam must follow gate')
    scale, color = sprite['_scale'], sprite['_color']
    origin = project((0, 0, 0), chain)
    x = project((scale['x'], 0, 0), chain)
    y = project((0, scale['y'], 0), chain)
    renderer = json.loads((source / 'level186-3454.json').read_text())
    values = [x[0]-origin[0], y[0]-origin[0], origin[0], x[1]-origin[1], y[1]-origin[1], origin[1], origin[2],
              color['r'], color['g'], color['b'], color['a'], renderer['m_SortingLayer'], renderer['m_SortingOrder'],
              x[2]-origin[2], y[2]-origin[2], 0]
    require(all(math.isfinite(v) for v in values), 'Invalid City slam transform')
    return sprites, compiled_clip, struct.pack('<4s2I16f', b'CCS1', first, 97, *values)
