"""Admit the Grove captive's original tk2d sprite into the ordinary room atlas.

Approximation: use the first authored Idle frame. Bottle rescue currently uses
the existing pickup presentation (hide captive/bottle immediately on rescue).
"""
import hashlib
import sys

from PIL import Image
from assetripper_client import ROOT
from convert_hero import decode_sprite


def add_tile(source, mapping, tiles, by_sprite):
    if mapping['scene'] != 'Waterways_13':
        return None
    # UnityPy is private tooling, never a runtime dependency.
    sys.path.insert(0, str(ROOT/'_private/tools/boss-python'))
    import UnityPy
    from watcher_unity import AlignedGenerator
    original = ROOT/'_private/original/Hollow Knight/hollow_knight_Data'
    env = UnityPy.Environment(path=str(original))
    generator = AlignedGenerator('6000.0.61f1')
    # Resolve the two known tk2d types directly. Resolving their MonoScript
    # pointers makes UnityPy discover unrelated shared assets and assemblies.
    for assembly in ('netstandard.dll', 'mscorlib.dll',
                     'UnityEngine.CoreModule.dll', 'TeamCherry.TK2D.dll'):
        generator.load_dll((original/'Managed'/assembly).read_bytes())
    env.load_file(str(original/'sharedassets33.assets'))
    # Preload the atlas stream: UnityPy's missing-resource fallback opens every
    # external asset before trying the requested stream.
    env.load_file(str(original/'sharedassets33.assets.resS'))
    bank = next(f for f in env.files.values() if getattr(f, 'name', None) == 'sharedassets33.assets')
    collection = bank.objects[883].read_typetree(
        nodes=generator.get_nodes_up('TeamCherry.TK2D', 'tk2dSpriteCollectionData'))
    library = bank.objects[963].read_typetree(
        nodes=generator.get_nodes_up('TeamCherry.TK2D', 'tk2dSpriteAnimation'))
    clip = next(c for c in library['clips'] if c['name'] == 'Idle')
    frame = clip['frames'][0]
    if (frame['spriteCollection'] != {'m_FileID': 0, 'm_PathID': 883}
            or collection['textures'] != [{'m_FileID': 0, 'm_PathID': 12}]):
        raise ValueError('Changed Grove grub collection or atlas')
    atlas = bank.objects[12].read().image.convert('RGBA')
    bitmap, geometry = decode_sprite(atlas, collection['spriteDefinitions'][frame['spriteId']])
    # Source 64 texels/unit -> 25.6, above the native 3DS projection. Keep
    # geometry and source UV orientation; filter premultiplied pixels.
    bitmap = bitmap.convert('RGBa').resize(
        (max(1, round(bitmap.width*.4)), max(1, round(bitmap.height*.4))),
        Image.Resampling.BOX).convert('RGBA')
    if bitmap.width*bitmap.height*4 > 256*1024:
        raise ValueError('Grove grub artwork exceeds its 256 KiB budget')
    key = '@grove-grub-idle'
    index = len(tiles)
    tiles.append(dict(bitmap=bitmap, geometry=geometry, source=key, index=index))
    by_sprite[key] = [index]
    return dict(key=key, renderer='level362-4352.json', owner=44,
                sprite='level362-6493.json', chain=['level362-2212.json', 'level362-2211.json'],
                source_sha256=hashlib.sha256((original/'sharedassets33.assets').read_bytes()).hexdigest(),
                clip='Idle', frame=frame['spriteId'], approximation='Static first Idle frame')


def add_draw(art, draws, by_sprite, read, project):
    if art is None:
        return
    sprite = read(art['sprite'])['m_Structure']
    if sprite['collection'] != {'m_FileID': 4, 'm_PathID': 883}:
        raise ValueError('Changed Grove captive collection')
    chain = [read(name) for name in art['chain']]
    origin = project((0, 0, 0), chain)
    px, py = project((1, 0, 0), chain), project((0, 1, 0), chain)
    scale, color = sprite['_scale'], sprite['_color']
    a, c, zx = [(px[i]-origin[i])*scale['x'] for i in range(3)]
    b, d, zy = [(py[i]-origin[i])*scale['y'] for i in range(3)]
    renderer = read(art['renderer'])
    index, owner = by_sprite[art['key']][0], art['owner']
    layer, order = renderer['m_SortingLayer'], renderer['m_SortingOrder']
    draws.append(dict(sprite=index, owner=owner,
        values=[a, b, origin[0], c, d, origin[1], origin[2],
                *[color[k] for k in ('r', 'g', 'b', 'a')], layer, order, zx, zy, 0],
        sort=(layer, order, -origin[2], owner, index)))
