"""Convert Town Elderbug's original default animation to a PSP CLUT8 bank."""
import json
import struct
import sys
from pathlib import Path
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parent))
from transcode_anim_libraries import Original, write_bank
import transcode_room as room
from world_quest_assets import Asset, RESOURCES_SHA, decrypt_language, language_asset
from mushroom_dialogue_layout import LAYOUT


def reflow(value, advances):
    value = value.replace('<br>', '\n').replace('’', "'").replace('“', '"').replace('”', '"')

    def fits(text):
        return sum(advances[ord(char)] for char in text) * LAYOUT['scale'] <= LAYOUT['width']

    for page in value.split('<page>'):
        lines = []
        for paragraph in page.strip().split('\n'):
            line = ''
            for word in paragraph.split():
                candidate = (line + ' ' + word).strip()
                if fits(candidate):
                    line = candidate
                else:
                    if not line or not fits(word):
                        raise ValueError('Elderbug dialogue contains an oversized word')
                    lines.append(line)
                    line = word
            lines.append(line)
        for start in range(0, len(lines), 5):
            text = '\n'.join(lines[start:start+5]).strip()
            if text:
                yield text


def write_dialogue(original, source_root, output, transform, npc='elderbug'):
    tiso = npc == 'tiso'
    resources = Asset(original.source/'resources.assets')
    if resources.sha256 != RESOURCES_SHA:
        raise ValueError('Unexpected original Elderbug language source')
    _, encoded = language_asset(resources.objects, 'EN_Minor NPC' if tiso else 'EN_Elderbug')
    entries = {entry.attrib['name']: entry.text for entry in ET.fromstring(decrypt_language(encoded))}
    font = json.loads((source_root/'_private/research/tutorial-reminders/prompt-map.json').read_text())['fonts']['sharedassets0.assets-194.json']
    advances = {glyph['id']: glyph['xAdvance']*16/font['m_fontInfo']['PointSize'] for glyph in font['m_glyphInfoList']}
    advances[32] = 4
    keys = ('INTRO_NORMAL', 'INTRO_WALKEDBY', 'INTRO_VISITEDCROSSROAD', 'INTRO_MAIN', 'GENERIC_2',
            'BLACK_EGG_OPENED', 'CROSSROADS_INFECTED', 'SHOPKEEP_RETURNED', 'STAG_STATION_REOPENED',
            'TEMPLE_VISITED', 'MAP_SHOP_OPEN', 'BRETTA_RETURNED', 'JIJI_DOOR', 'KINGS_PASS', 'MINES_LIFT',
            'HISTORY_1', 'HISTORY_2', 'HINT_RESTING_GROUNDS', 'HINT_ANCIENT_BASIN', 'HINT_CRYSTAL_PEAK',
            'HINT_CITY_OF_TEARS', 'HINT_FUNGAL_WASTES', 'HINT_GREENPATH')
    if tiso:
        keys = ('TOWN_GREET', 'TOWN_REPEAT')
    texts = ['<page>'.join(reflow(entries[npc.upper()+'_'+key], advances)).encode('ascii')
             for key in keys]
    if any(not text or len(text) >= 768 or b'\0' in text for text in texts):
        raise ValueError('Elderbug dialogue exceeds bounded storage')
    owner, transform_id, collider_id = (155, 1341, 2503) if tiso else (1001, 2188, 2586)
    collider = original.load('level7').objects[collider_id].read_typetree()
    if collider['m_GameObject']['m_PathID'] != owner or not collider['m_IsTrigger']:
        raise ValueError('Unexpected Elderbug trigger or transform')
    scene = Asset(original.source/'level7')
    position = scene.point(transform_id, (0, 0))
    scale = transform['m_LocalScale']
    points = [scene.point(transform_id, (collider['m_Offset']['x']+x*collider['m_Size']['x']/2,
                                collider['m_Offset']['y']+y*collider['m_Size']['y']/2))
              for x in (-1, 1) for y in (-1, 1)]
    bounds = [min(p[0] for p in points), min(p[1] for p in points),
              max(p[0] for p in points), max(p[1] for p in points)]
    data = struct.pack('<4sIII32s8fII', b'WQ05' if tiso else b'WQ04', 4 if tiso else 3,
                       7000000+owner, owner, b'Town', *bounds, *position, scale['x'], scale['y'], 1, owner)
    data += b''.join(struct.pack('<I', len(text))+text for text in texts)
    (output/f'{npc}.bin').write_bytes(data)


def convert(source_root, output, npc='elderbug'):
    original = Original(source_root)
    scene = original.load('level7')
    owner, animator_id, transform_id = (155, 3810, 1341) if npc == 'tiso' else (1001, 3820, 2188)
    animator = scene.objects[animator_id].read_typetree()
    transform = scene.objects[transform_id].read_typetree()
    if any(part['m_GameObject']['m_PathID'] != owner for part in (animator, transform)):
        raise ValueError('Unexpected Town Elderbug owner')
    library = original.resolve(scene, animator['library'])
    idle = library.read_typetree()['clips'][animator['defaultClipId']]
    if 'idle' not in idle['name'].lower() or not idle['frames']:
        raise ValueError(f'Unexpected Elderbug default clip: {idle["name"]}')
    write_idle(original, output, npc, library, idle, transform['m_LocalScale'])
    write_dialogue(original, source_root, output, transform, npc)


def write_idle(original, output, npc, library, idle, scale):
    sprites = {}
    keys = []
    for frame in idle['frames']:
        ref = frame['spriteCollection']
        key = (ref['m_FileID'], ref['m_PathID'], frame['spriteId'])
        if key not in sprites:
            sprites[key] = original.sprite(library.assets_file, frame)
        keys.append(key)

    image = room.import_pil()
    places, count = room.pack([sprite.size for sprite, _ in sprites.values()], page=2048)
    source_pages = {index: image.new('RGBA', (2048, 2048)) for index in range(count)}
    records = []
    factors = []
    density = room.room_focal_length(480, 272) / 38.1
    for (sprite, geometry), (page, left, top) in zip(sprites.values(), places):
        room.paste_padded(source_pages[page], sprite, left, top)
        records.append((page, left, top, sprite.width, sprite.height, *geometry))
        x0, y0, x1, y1 = geometry
        factors.append((min(1., (x1-x0)*density*abs(scale['x'])/sprite.width),
                        min(1., (y1-y0)*density*abs(scale['y'])/sprite.height)))
    output.mkdir(parents=True, exist_ok=True)
    _, pages, _, geometry = room.build_pages(image, output, '', 0, records, factors,
                                            output, extra_pages=source_pages)
    if pages != 1:
        raise ValueError(f'{npc} idle exceeds the one-page budget: {pages}')
    clips, frames, events = write_bank([(f'{npc.title()} Idle', idle, keys)], sprites, pages)
    table = (struct.pack('<4s6I', b'PSA1', 1, room.PAGE, pages, len(sprites), 1, len(frames))
             + geometry + clips + struct.pack(f'<{len(frames)}I', *frames) + events)
    (output/f'{npc}-visual.bin').write_bytes(table)
    print(f'{npc}: {idle["name"]}, {len(frames)} frames, {len(sprites)} sprites, {pages} page')


if __name__ == '__main__':
    convert(room.source_root(), room.REPO/'_private/converted-psp/elderbug')
