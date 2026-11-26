"""Compile the evidenced Sly and Bretta rescue slices from immutable copied build 22529139.

FSM120 Meet sets slyRescued before SLY_RESCUE_1; CONVO_FINISH advances to
Meet 2, Meet 3, Talk Finish. Active? destroys the hierarchy on saved rescue.
Decoded evidence: _private/minimum-world-quests/level20-120-fsm.json.
The source digest pins that observation; changed originals need a new review.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import xml.etree.ElementTree as ET

from pickup_assets import Asset
from extract_tutorial_prompts import decrypt_language
from mushroom_dialogue_layout import wrap_page

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / '_private/original/Hollow Knight/hollow_knight_Data'
SOURCE_SHA = '65ca302a1b76df1fefeb4e71d1b5bf8afd803578cd5557dcdfa8848373cbe7de'
RESOURCES_SHA = '6d49ac5a50a282a645acc32518b28e7a29d0b94a0c3b5a4cda18dade3e6fe511'
KEYS = ('SLY_RESCUE_1', 'SLY_RESCUE_2', 'SLY_RESCUE_3', 'SLY_RESCUE_REPEAT')
BRETTA_SOURCE_SHA = '95ad359880ec2c5ab462e4de3c96f3b18c174522547311329b05b47ef6bc0d38'


def quest_spec(quest):
    if quest == 'sly':
        return dict(name='Sly Dazed', sha=SOURCE_SHA, level=20, owner=26, trigger=87,
                    fsm=120, scene='Room_ruinhouse', sheet='EN_Sly', keys=KEYS,
                    event=1, magic=b'WQ03', field='slyRescued', output='world-quests.bin')
    if quest == 'bretta':
        return dict(name='Bretta Dazed', sha=BRETTA_SOURCE_SHA, level=187, owner=191,
                    trigger=4290, fsm=5960, scene='Fungus2_23', sheet='EN_Minor NPC',
                    keys=tuple('BRETTA_RESCUE_'+str(i) for i in range(1,6)), event=2,
                    magic=b'WQ04', field='brettaRescued', output='bretta-quest.bin')
    raise ValueError('Unsupported source quest: '+str(quest))



def language_asset(objects, name):
    """Read one original TextAsset; reject ambiguous or truncated localization."""
    matches = []
    for owner, (kind, data) in objects.items():
        if kind != 49:
            continue
        if len(data) < 4:
            raise ValueError('Truncated TextAsset name')
        length = struct.unpack_from('<i', data)[0]
        if length < 0 or length > len(data)-4:
            raise ValueError('Invalid TextAsset name length')
        if data[4:4+length].decode('utf-8') != name:
            continue
        offset = (4+length+3)//4*4
        if offset+4 > len(data):
            raise ValueError('Truncated language TextAsset')
        size = struct.unpack_from('<i', data, offset)[0]
        if size < 0 or size > len(data)-offset-4:
            raise ValueError('Invalid language TextAsset length')
        matches.append((owner, data[offset+4:offset+4+size].decode('utf-8')))
    if len(matches) != 1:
        raise ValueError(f'Expected one original {name} TextAsset, found {len(matches)}')
    return matches[0]


def compile_quest(source, dialogue=None, resources=DATA/'resources.assets', quest='sly'):
    spec = quest_spec(quest)
    asset = Asset(source)
    if asset.sha256 != spec['sha']:
        raise ValueError('Source quest build differs from the reviewed FSM')
    go = asset.gos[spec['owner']]
    if go['name'] != spec['name'] or not go['active'] or spec['fsm'] not in go['components']:
        raise ValueError('Missing reviewed Sly source owner')
    kind, collider = asset.objects[spec['trigger']]
    if kind != 61 or len(collider) != 148 or spec['trigger'] not in go['components']:
        raise ValueError('Missing reviewed Sly trigger')
    x, y = struct.unpack_from('<2f', collider, 72)
    width, height = struct.unpack_from('<2f', collider, 136)
    transform = asset.transform(spec['owner'])
    points = [asset.point(transform, (x+dx*width/2, y+dy*height/2))
              for dx in (-1, 1) for dy in (-1, 1)]
    bounds = (min(p[0] for p in points), min(p[1] for p in points),
              max(p[0] for p in points), max(p[1] for p in points))
    if not all(math.isfinite(v) for v in bounds) or bounds[0] >= bounds[2] or bounds[1] >= bounds[3]:
        raise ValueError('Invalid source interaction trigger')
    owners = [spec['owner'], *asset.descendants(transform)]
    if len(owners) > 16 or len(set(owners)) != len(owners):
        raise ValueError('Unsupported Sly source hierarchy')
    language_source = {}
    if dialogue is None:
        language = Asset(resources)
        if language.sha256 != RESOURCES_SHA:
            raise ValueError('Sly language source differs from reviewed build 22529139')
        language_owner, encoded = language_asset(language.objects, spec['sheet'])
        xml = decrypt_language(encoded).encode('utf-8')
        language_source = dict(resources_sha256=language.sha256, text_asset=language_owner)
    else:
        xml = Path(dialogue).read_bytes()
    entries = {e.attrib['name']: e.text for e in ET.fromstring(xml)}
    language_source['dialogue_sha256'] = hashlib.sha256(xml).hexdigest()
    font = json.loads((ROOT/'_private/research/tutorial-reminders/prompt-map.json').read_text())['fonts']['sharedassets0.assets-194.json']
    advances = {g['id']:g['xAdvance']*16/font['m_fontInfo']['PointSize'] for g in font['m_glyphInfoList']}
    advances[32] = 4
    texts = ['<page>'.join(wrap_page(page, advances) for page in entries[key].split('<page>')).encode('ascii') for key in spec['keys']]
    if any(not text or len(text) >= 768 or b'\0' in text for text in texts):
        raise ValueError('Source rescue dialogue exceeds bounded storage')
    position = asset.point(transform, (0, 0))
    scale = struct.unpack_from('<2f', asset.transforms[transform][2], 40)
    data = struct.pack('<4sIII32s8f', spec['magic'], spec['event'], spec['level']*1_000_000+spec['owner'], spec['owner'], spec['scene'].encode('ascii'), *bounds, *position, *scale)
    data += struct.pack('<I', len(owners)) + struct.pack('<'+'I'*len(owners), *owners)
    data += b''.join(struct.pack('<I', len(text))+text for text in texts)
    manifest = dict(source_sha256=asset.sha256, source_scene=spec['level'], scene=spec['scene'],
                    owner=spec['owner'], trigger=spec['trigger'], fsm=spec['fsm'], range=bounds, hierarchy=owners,position=position,scale=scale,
                    dialogue_keys=spec['keys'], source_field=spec['field'], commit_state='Meet',
                    commit_timing='before first dialogue', relocation='next scene initialization')
    manifest['language_source'] = language_source
    manifest['binary_sha256'] = hashlib.sha256(data).hexdigest()
    return data, manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--quest', choices=('sly', 'bretta'), default='sly')
    parser.add_argument('--source', type=Path)
    parser.add_argument('--dialogue', type=Path, help='Optional already-decrypted source XML override')
    parser.add_argument('--resources', type=Path, default=DATA/'resources.assets')
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    spec = quest_spec(args.quest)
    args.source = args.source or DATA/('level'+str(spec['level']))
    args.out = args.out or ROOT/'_private/converted/hero/romfs'/spec['output']
    converted = ROOT/'_private/converted'
    if converted.is_symlink() or converted.is_junction() or not args.out.resolve().is_relative_to(converted.resolve()):
        parser.error('Quest output must be in this worktree real converted directory')
    data, manifest = compile_quest(args.source, args.dialogue, args.resources, args.quest)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(data)
    provenance = converted/'world-quests'/('source.json' if args.quest == 'sly' else 'bretta-source.json')
    provenance.parent.mkdir(parents=True, exist_ok=True)
    provenance.write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
    print(f'{args.quest} quest: {len(data)} bytes, {len(manifest["hierarchy"])} source owners')


if __name__ == '__main__':
    main()
