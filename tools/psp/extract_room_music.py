"""Extract original Crossroads music stems to PSP-local WAV (Python 3.12).

Called by transcode_audio, including regen-assets -Only audio. Original inputs
are read-only; the Normal snapshot is the only admitted mix contract.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import wave


CUE_SHA256 = 'a39f70c401eefe27eee2741f7eb57d7b2a9e609f62cb58d4c3854ab1859f1bbd'


def extract(source, output):
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(source/'_private/tools/boss-python'))
    import UnityPy
    original = source/'_private/original/Hollow Knight/hollow_knight_Data'
    music = UnityPy.load(str(original/'sharedassets19.assets'))
    objects = {o.path_id: o for o in music.objects}
    cue = objects[86].get_raw_data()
    if hashlib.sha256(cue).hexdigest() != CUE_SHA256:
        raise ValueError('Crossroads MusicCue changed: re-admit channels and synchronization')
    resources = UnityPy.load(str(original/'resources.assets'))
    resources = {o.path_id: o for o in resources.objects}
    snapshot = resources[3855].read_typetree()
    mixer = resources[3809].read_typetree()
    constant = mixer['m_MixerConstant']
    names = [n.decode('ascii') for n in bytes(constant['groupNameBuffer']).split(b'\0') if n]
    if (snapshot['m_Name'] != 'Normal' or snapshot['m_AudioMixer']['m_PathID'] != 3809 or
            constant['snapshotGUIDs'][0] != snapshot['m_SnapshotID'] or
            names != ['Master', 'Main', 'Main Alt', 'Action', 'Sub', 'Tension', 'Extra']):
        raise ValueError('Original Normal music snapshot identity changed')
    gains = {name: constant['snapshots'][0]['values'][group['volumeIndex']]
             for name, group in zip(names, constant['groups'])}
    if gains['Master'] != 0 or gains['Main'] != 0 or gains['Action'] != -60 or gains['Tension'] != -60:
        raise ValueError('Original Normal music gains changed')
    output.mkdir(parents=True, exist_ok=True)
    clips = []
    for path_id, name, stem, rate, frames, group in (
            (27, 'S19 Crossroads Main', 'main', 44100, 6773761, 'Main'),
            (34, 'S19 Action', 'action', 44100, 6773761, 'Action'),
            (26, 'S19 Optional Drone', 'tension', 48000, 257280, 'Tension')):
        clip = objects[path_id].read()
        if (clip.m_Name, clip.m_Channels, clip.m_Frequency) != (name, 2, rate):
            raise ValueError('Original music clip identity changed')
        samples = clip.samples
        if set(samples) != {name+'.wav'}:
            raise ValueError('Unexpected decoded music samples')
        wav = output/f'music-crossroads-{stem}.wav'
        wav.write_bytes(samples[name+'.wav'])
        with wave.open(str(wav), 'rb') as f:
            if (f.getnchannels(), f.getsampwidth(), f.getframerate(), f.getnframes(), f.getcomptype()) != (
                    2, 2, rate, frames, 'NONE'):
                raise ValueError('Original music duration/PCM changed')
        clips.append(dict(collection='sharedassets19.assets', path_id=path_id, name=name,
                          file=wav.name, sha256=hashlib.sha256(wav.read_bytes()).hexdigest(),
                          frames=frames, rate=rate, gain_db=gains[group],
                          sync=2 if stem=='tension' else 1))
    hashes = {name: hashlib.sha256((original/name).read_bytes()).hexdigest() for name in (
        'sharedassets19.assets', 'sharedassets19.resource', 'resources.assets')}
    manifest = dict(cue='sharedassets19.assets:86', cue_sha256=CUE_SHA256,
                    snapshot='resources.assets:3855', snapshot_name='Normal',
                    source_hashes=hashes, clips=clips,
                    phase='Main/Action synchronize; Tension sync=2 loops independently',
                    remaining='Atmosphere omissions and original snapshot transitions')
    (output/'room-music.json').write_text(json.dumps(manifest, indent=2)+'\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    extract(args.source, args.out)
