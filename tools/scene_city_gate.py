"""Original Fungus2_21 gate geometry/text; embedded in its scene transaction."""
import struct
import xml.etree.ElementTree as ET
from pathlib import Path
from pickup_assets import Asset
from convert_route_tolls import descendants, region
from world_quest_assets import language_asset
from extract_tutorial_prompts import decrypt_language

ROOT = Path(__file__).resolve().parents[1]

def export_city_gate(target):
    data = ROOT / '_private/original/Hollow Knight/hollow_knight_Data'
    source = Asset(data / 'level186')
    assert source.sha256 == '24149341cfb0ae0cf089c2a69b3cfc5a3f4b5c51b305535223945dbf6ccc88c0', 'Unreviewed City gate source build'
    # Conversation Control 5567; Open 5613; trigger FSMs 5561/5566.
    regions = [region(source, i) for i in (3806, 3804, 3799)]
    respawn = source.point(source.transform(397), (0, 0))
    binary = struct.pack('<4s14f', b'CGT1', *(v for r in regions for v in r), *respawn)
    for identity in (3830, 3837):
        binary += struct.pack('<I4f', identity, *region(source, identity))
    owners = descendants(source, 1466)
    binary += struct.pack('<I', len(owners)) + struct.pack('<' + 'I'*len(owners), *owners)
    resources = Asset(data / 'resources.assets')
    _, encoded = language_asset(resources.objects, 'EN_Prompts')
    root = ET.fromstring(decrypt_language(encoded))
    for key in ('CITY_GATE_NOKEY', 'CITY_GATE_KEY'):
        text = next(e.text for e in root if e.get('name') == key).replace('<br>', ' ')
        raw = text.encode('utf-8')
        assert len(raw) < 256
        binary += raw + b'\0'*(256-len(raw))
    (target / 'city-gate.bin').write_bytes(binary)
    export_audio(target)


def export_audio(target):
    # Read archived original clips; only the room's converted output is written.
    import sys, io, wave, array
    sys.path.insert(0, str(ROOT / '_private/tools/boss-python'))
    sys.path.insert(0, str(ROOT / '_private/tools/audio-python'))
    import UnityPy
    clips = (('sharedassets7.assets',108,'manhole_key_turn'),
             ('resources.assets',1337,'shiny_item_pickup'),
             ('sharedassets7.assets',107,'jiji_door_open_pt_1'),
             ('sharedassets7.assets',98,'jiji_door_open_pt_2'),
             ('sharedassets6.assets',161,'stalactite_break'),
             ('sharedassets6.assets',171,'false_knight_land_1st_time'))
    files = {}
    samples = []
    for collection, identity, name in clips:
        if collection not in files:
            asset = ROOT / '_private/original/Hollow Knight/hollow_knight_Data' / collection
            # Byte loading avoids UnityPy scanning unrelated dependencies in the archive.
            env = UnityPy.Environment()
            resource = asset.with_suffix('.resource')
            env.load_file(resource.read_bytes(), name=resource.name)
            files[collection] = env.load_file(asset.read_bytes(), name=collection)
        clip = files[collection].objects[identity].read()
        assert clip.m_Name == name and len(clip.samples) == 1
        with wave.open(io.BytesIO(next(iter(clip.samples.values()))),'rb') as wav:
            assert wav.getsampwidth() == 2 and wav.getframerate() == 44100 and wav.getnchannels() in (1,2)
            channels = wav.getnchannels()
            pcm = array.array('h',wav.readframes(wav.getnframes()))
        # Approximation: mono downmix for the bounded single-channel gate streamer.
        if channels == 2:pcm = array.array('h',((pcm[i]+pcm[i+1])//2 for i in range(0,len(pcm),2)))
        samples.append(pcm)
    insert = array.array('h',(max(-32768,min(32767,sum(p[i] if i<len(p) else 0 for p in samples[:2])))
                              for i in range(max(len(p) for p in samples[:2]))))
    for identity, pcm in zip((1,2,3,4,6),(insert,*samples[2:])):
        with wave.open(str(target / f'city-gate-{identity}.wav'),'wb') as wav:
            wav.setparams((1,2,44100,len(pcm),'NONE','not compressed'));wav.writeframes(pcm.tobytes())
