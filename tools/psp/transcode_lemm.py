"""Extract the Relic Dealer's original idle, triggers and dialogue."""
import json
import struct
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from transcode_elderbug import Asset, Original, RESOURCES_SHA, decrypt_language, language_asset, reflow, room, write_idle


def bounds(asset, transform, collider):
    offset, size = collider['m_Offset'], collider['m_Size']
    corners = [asset.point(transform, (offset['x'] + x * size['x'] / 2, offset['y'] + y * size['y'] / 2))
               for x in (-1, 1) for y in (-1, 1)]
    return tuple(min(point[axis] for point in corners) for axis in (0, 1)) + tuple(
        max(point[axis] for point in corners) for axis in (0, 1))


def convert(source, output):
    original = Original(source)
    scene = original.load('level93')
    transform, animator, talk, shop = (scene.objects[key].read_typetree() for key in (829, 2632, 1790, 1788))
    assert all(item['m_GameObject']['m_PathID'] == 17 for item in (transform, animator, talk))
    assert shop['m_GameObject']['m_PathID'] == 11 and talk['m_IsTrigger'] and shop['m_IsTrigger']
    asset = Asset(original.source/'level93')
    library = original.resolve(scene, animator['library'])
    idle = library.read_typetree()['clips'][animator['defaultClipId']]
    resources = Asset(original.source/'resources.assets')
    if resources.sha256 != RESOURCES_SHA:
        raise ValueError('Unexpected Relic Dealer language source')
    _, encoded = language_asset(resources.objects, 'EN_Relic Dealer')
    entries = {entry.attrib['name']: ''.join(entry.itertext()) for entry in ET.fromstring(decrypt_language(encoded))}
    font = json.loads((source/'_private/research/tutorial-reminders/prompt-map.json').read_text())['fonts']['sharedassets0.assets-194.json']
    advances = {glyph['id']: glyph['xAdvance']*16/font['m_fontInfo']['PointSize'] for glyph in font['m_glyphInfoList']}
    advances[32] = 4
    keys = ('SHOP_INTRO', 'CONVO_1', 'TALK', 'TALK_2', 'TALK_REPEAT', 'DUNG', 'NAILSMITH', 'NO_RELICS')
    texts = ['<page>'.join(reflow(entries['RELICDEALER_'+key], advances)).encode('ascii') for key in keys]
    if any(not text or len(text) >= 768 or b'\0' in text for text in texts):
        raise ValueError('Relic Dealer dialogue exceeds bounded storage')
    scale = transform['m_LocalScale']
    write_idle(original, output, 'lemm', library, idle, scale)
    header = struct.pack('<4sIII32s8fII4f', b'WQ08', 7, 93000017, 17, b'Ruins1_05b',
                         *bounds(asset, 829, talk), *asset.point(829, (0, 0)), scale['x'], scale['y'],
                         1, 17, *bounds(asset, 823, shop))
    (output/'lemm.bin').write_bytes(header + b''.join(struct.pack('<I', len(text))+text for text in texts))


if __name__ == '__main__':
    convert(room.source_root(), room.REPO/'_private/converted-psp/lemm')
