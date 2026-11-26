"""Recover only the five source gate clips; run under validation-lock -Exclusive."""
import hashlib
import io
import json
from pathlib import Path
import sys
import wave

from assetripper_client import ROOT


def main():
    sys.dont_write_bytecode = True
    sys.path[:0] = [str(ROOT / '_private/tools/boss-python'), str(ROOT / '_private/tools/audio-python')]
    import UnityPy
    source = ROOT / '_private/original/Hollow Knight/hollow_knight_Data'
    output = ROOT / '_private/minimum-obstacles/city-crest-audio'
    output.mkdir(parents=True, exist_ok=True)
    scene = next(iter(UnityPy.load(str(source / 'level186')).files.values()))
    if [str(e.path) for e in scene.externals[1:3]] != ['sharedassets6.assets', 'sharedassets7.assets']:
        raise ValueError('Changed level186 audio collections')
    files, entries = {}, []
    clips = ((7,107,'open','jiji_door_open_pt_1'), (7,100,'move','lift_moving_loop'),
             (7,98,'bump','jiji_door_open_pt_2'), (6,161,'drop','stalactite_break'),
             (6,171,'slam','false_knight_land_1st_time'))
    for collection, ident, role, name in clips:
        if collection not in files:
            files[collection] = next(iter(UnityPy.load(str(source / f'sharedassets{collection}.assets')).files.values()))
        clip = files[collection].objects[ident].read()
        samples = clip.samples
        if clip.m_Name != name or len(samples) != 1:
            raise ValueError('Changed original City Crest clip')
        raw = next(iter(samples.values()))
        with wave.open(io.BytesIO(raw)) as wav:
            rate, channels, frames, width = wav.getframerate(), wav.getnchannels(), wav.getnframes(), wav.getsampwidth()
        (output / f'{role}.wav').write_bytes(raw)
        entries.append(dict(collection=collection, path_id=ident, role=role, name=name,
                            rate=rate, channels=channels, frames=frames, width=width,
                            sha256=hashlib.sha256(raw).hexdigest()))
    (output / 'source-report.json').write_text(json.dumps(dict(clips=entries), indent=2) + '\n')
    print(f'Recovered {len(entries)} original City Crest gate clips')


if __name__ == '__main__':
    main()
