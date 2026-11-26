"""Extract admitted Cave/Surface rain sources to PSP-local WAV (Python 3.12).

Called by transcode_audio, including regen-assets -Only audio. Original files
are read-only. Both cues enable sources 5/6 with identical snapshot gains.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import wave


CAVE_CUE_SHA256 = 'e13c860e6bf1810eddcfc29b5b1a26ff7b6513eaf1db9ff400bfcd723dc22da6'
SURFACE_CUE_RAW = bytes.fromhex(
    '000000000000000000000000010000000100000079040000000000000700000053757266616365'
    '0004000000320f0000000000001000000000010001000101000000000000000000')
RAIN_DB = -60.40684127807617


def extract(source, output):
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(source/'_private/tools/boss-python'))
    import UnityPy
    original = source/'_private/original/Hollow Knight/hollow_knight_Data'
    cave = UnityPy.load(str(original/'sharedassets6.assets'))
    cue = next(o for o in cave.objects if o.path_id==1054).get_raw_data()
    surface = UnityPy.load(str(original/'sharedassets5.assets'))
    surface_cue = next(o for o in surface.objects if o.path_id==14).get_raw_data()
    if hashlib.sha256(cue).hexdigest()!=CAVE_CUE_SHA256 or surface_cue!=SURFACE_CUE_RAW:
        raise ValueError('Original Cave/Surface AtmosCue changed: re-admit rain sources')
    env = UnityPy.load(str(original/'resources.assets'))
    objects = {o.path_id: o for o in env.objects}
    mixer = objects[3805].read_typetree()['m_MixerConstant']
    names = [n.decode('ascii') for n in bytes(mixer['groupNameBuffer']).split(b'\0') if n]
    gains = []
    for pid, name in ((3852, 'at Cave'), (3890, 'at Surface')):
        snapshot = objects[pid].read_typetree()
        if snapshot['m_Name']!=name or snapshot['m_AudioMixer']['m_PathID']!=3805:
            raise ValueError('Original atmosphere snapshot identity changed')
        index = mixer['snapshotGUIDs'].index(snapshot['m_SnapshotID'])
        values = mixer['snapshots'][index]['values']
        groups = {n: values[g['volumeIndex']] for n, g in zip(names, mixer['groups'])}
        if groups['Master']!=0 or any(groups[n]!=RAIN_DB for n in ('Rain Indoor', 'Rain Outdoor')):
            raise ValueError('Original Cave/Surface rain gains changed')
        gains.append(dict(snapshot=f'resources.assets:{pid}', name=name, gain_db=RAIN_DB))
    output.mkdir(parents=True, exist_ok=True)
    clips = []
    for channel, source_id, path_id, group_id, group, name, frames in (
            (5, 13881, 1284, 3845, 'Rain Indoor', 'ruins_rain_indoor_loop', 307244),
            (6, 13886, 1276, 3846, 'Rain Outdoor', 'ruins_rain_outdoor_loop', 441001)):
        audio_source = objects[source_id].read_typetree()
        mixer_group = objects[group_id].read_typetree()
        if (audio_source['m_Resource']!={'m_FileID': 0, 'm_PathID': path_id} or
                audio_source['OutputAudioMixerGroup']!={'m_FileID': 0, 'm_PathID': group_id} or
                audio_source['m_Volume']!=1 or audio_source['m_Pitch']!=1 or
                not audio_source['Loop'] or audio_source['Mute'] or audio_source['Pan2D']!=0 or
                mixer_group['m_Name']!=group or mixer_group['m_AudioMixer']['m_PathID']!=3805):
            raise ValueError('Original rain source/group routing changed')
        clip = objects[path_id].read()
        if (clip.m_Name, clip.m_Channels, clip.m_Frequency)!=(name, 2, 44100):
            raise ValueError('Original rain clip identity changed')
        samples = clip.samples
        if set(samples)!={name+'.wav'}:
            raise ValueError('Unexpected decoded rain samples')
        path = output/(name+'.wav');path.write_bytes(samples[path.name])
        with wave.open(str(path), 'rb') as wav:
            if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate(), wav.getnframes(),
                    wav.getcomptype())!=(2, 2, 44100, frames, 'NONE'):
                raise ValueError('Original rain duration/PCM changed')
        clips.append(dict(collection='resources.assets', path_id=path_id, source_id=source_id,
                          channel=channel, group_id=group_id, name=name, file=path.name,
                          sha256=hashlib.sha256(path.read_bytes()).hexdigest(), rate=44100,
                          frames=frames, gain_db=RAIN_DB))
    manifest = dict(cues=['sharedassets6.assets:1054', 'sharedassets5.assets:14'],
                    cue_sha256=[CAVE_CUE_SHA256, hashlib.sha256(surface_cue).hexdigest()],
                    snapshots=gains, clips=clips,
                    source_hashes={n: hashlib.sha256((original/n).read_bytes()).hexdigest() for n in (
                        'sharedassets5.assets', 'sharedassets6.assets', 'resources.assets')},
                    phase='Both rain sources keep playing across Cave/Surface cues',
                    remaining='Original snapshot transitions')
    (output/'room-atmosphere.json').write_text(json.dumps(manifest, indent=2)+'\n')


if __name__=='__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    extract(args.source, args.out)
