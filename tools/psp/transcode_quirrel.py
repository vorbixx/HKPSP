"""Convert the four route-side Quirrel encounters, excluding boss actors."""
import json
from pathlib import Path
import struct
import sys
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parent))
from transcode_elderbug import Asset, Original, reflow, room, write_idle


ENCOUNTERS = (
    ('Fungus2_01', 165, 1786, 3699, 5716, 3945, ('QUEENSTATION_01', 'QUEENSTATION_02', 'QUEENSTATION_REPEAT')),
    ('Fungus2_14', 178, 2019, 4498, 7463, 5155, ('MANTIS_01', 'MANTIS_REPEAT')),
    ('Ruins1_02', 89, 1179, 2551, 4449, 3018, ('RUINS_1', 'RUINS_2', 'RUINS_3', 'RUINS_4')),
    ('Fungus3_47', 223, 433, 1443, 2893, 2120, ('FOGCANYON_A', 'FOGCANYON_B')),
)


def convert(source, destination):
    original = Original(source)
    language = source/'_private/minimum-bosses/monomon-presentation/EN_Quirrel.xml'
    entries = {entry.attrib['name']: ''.join(entry.itertext()) for entry in ET.parse(language).iter('entry')}
    font = json.loads((source/'_private/research/tutorial-reminders/prompt-map.json').read_text())['fonts']['sharedassets0.assets-194.json']
    advances = {glyph['id']: glyph['xAdvance']*16/font['m_fontInfo']['PointSize'] for glyph in font['m_glyphInfoList']}
    advances[32] = 4
    for name, level, owner, transform_id, animator_id, trigger_id, keys in ENCOUNTERS:
        level_name = f'level{level}'
        scene = original.load(level_name)
        transform, animator, trigger = (scene.objects[id_].read_typetree() for id_ in (transform_id, animator_id, trigger_id))
        if any(part['m_GameObject']['m_PathID'] != owner for part in (transform, animator, trigger)) or not trigger['m_IsTrigger']:
            raise ValueError(f'Unexpected Quirrel placement in {name}')
        library = original.resolve(scene, animator['library'])
        clips = library.read_typetree()['clips']
        idle = clips[animator['defaultClipId']]
        if name == 'Ruins1_02':
            idle, = [clip for clip in clips if clip['name'] == 'Bench Idle']
        if not idle['frames']:
            raise ValueError(f'Missing Quirrel idle frames in {name}')
        scale = transform['m_LocalScale']
        asset = Asset(original.source/level_name)
        position = asset.point(transform_id, (0, 0))
        offset, size = trigger['m_Offset'], trigger['m_Size']
        corners = [asset.point(transform_id, (offset['x'] + x*size['x']/2, offset['y'] + y*size['y']/2))
                   for x in (-1, 1) for y in (-1, 1)]
        bounds = [min(point[0] for point in corners), min(point[1] for point in corners),
                  max(point[0] for point in corners), max(point[1] for point in corners)]
        texts = ['<page>'.join(reflow(entries['QUIRREL_'+key], advances)).encode('ascii') for key in keys]
        if any(not text or len(text) >= 768 or b'\0' in text for text in texts):
            raise ValueError(f'Quirrel dialogue exceeds bounded storage in {name}')
        output = destination/name
        write_idle(original, output, 'quirrel', library, idle, scale)
        header = struct.pack('<4sIII32s8fII', b'WQ06', 5, level*1000000+owner, owner,
                             name.encode('ascii'), *bounds, *position, scale['x'], scale['y'], 1, owner)
        (output/'quirrel.bin').write_bytes(header + b''.join(struct.pack('<I', len(text))+text for text in texts))


if __name__ == '__main__':
    convert(room.source_root(), room.REPO/'_private/converted-psp/quirrel')
