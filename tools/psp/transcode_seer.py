"""Extract the route Seer placement, idle and original dialogue."""
from pathlib import Path
import json
import struct
import sys
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parent))
from transcode_elderbug import Asset, Original, reflow, room, write_idle


def convert(source, output):
    original = Original(source)
    scene = original.load('level242')
    transform, animator, trigger = (scene.objects[id_].read_typetree() for id_ in (224, 566, 395))
    if any(part['m_GameObject']['m_PathID'] != 36 for part in (transform, animator, trigger)) or not trigger['m_IsTrigger']:
        raise ValueError('Unexpected Seer placement')
    library = original.resolve(scene, animator['library'])
    idle = library.read_typetree()['clips'][animator['defaultClipId']]
    scale = transform['m_LocalScale']
    asset = Asset(original.source/'level242')
    position = asset.point(224, (0, 0))
    offset, size = trigger['m_Offset'], trigger['m_Size']
    corners = [asset.point(224, (offset['x'] + x*size['x']/2, offset['y'] + y*size['y']/2))
               for x in (-1, 1) for y in (-1, 1)]
    bounds = [min(point[axis] for point in corners) for axis in (0, 1)]
    bounds += [max(point[axis] for point in corners) for axis in (0, 1)]
    language = source/'_private/research/conditional-events/EN_Dream Witch.xml'
    entries = {entry.attrib['name']: ''.join(entry.itertext()) for entry in ET.parse(language).iter('entry')}
    font = json.loads((source/'_private/research/tutorial-reminders/prompt-map.json').read_text())['fonts']['sharedassets0.assets-194.json']
    advances = {glyph['id']: glyph['xAdvance']*16/font['m_fontInfo']['PointSize'] for glyph in font['m_glyphInfoList']}
    advances[32] = 4
    intro = entries['WITCH_INTRO'].split('<page>')
    if len(intro) != 6:
        raise ValueError('Unexpected Seer intro page count')
    scripts = [entries['WITCH_MEET_A'], entries['WITCH_MEET_B'], '<page>'.join(intro[:3]),
               '<page>'.join(intro[3:]), entries['WITCH_GREET'], entries['WITCH_QUEST_1'],
               entries['WITCH_HINT_DREAMPLANT'], entries['WITCH_HINT_XERO'], entries['WITCH_GENERIC']]
    texts = ['<page>'.join(reflow(script, advances)).encode('ascii') for script in scripts]
    if any(not text or len(text) >= 768 or b'\0' in text for text in texts):
        raise ValueError('Seer dialogue exceeds bounded storage')
    write_idle(original, output, 'seer', library, idle, scale)
    header = struct.pack('<4sIII32s8fII', b'WQ07', 6, 242000036, 36, b'RestingGrounds_07',
                         *bounds, *position, scale['x'], scale['y'], 1, 36)
    (output/'seer.bin').write_bytes(header + b''.join(struct.pack('<I', len(text))+text for text in texts))


if __name__ == '__main__':
    convert(room.source_root(), room.REPO/'_private/converted-psp/seer')
