"""Decode admitted original movement loops into PSP-local WAVs (Python 3.12)."""
import argparse
from pathlib import Path
import sys


def extract(source, output):
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(source/'_private/tools/boss-python'))
    import UnityPy
    data = source/'_private/original/Hollow Knight/hollow_knight_Data'
    output.mkdir(parents=True, exist_ok=True)
    for bundle, path_id, name in [('sharedassets37.assets', 30, 'climer'),
                                  ('sharedassets33.assets', 22, 'fly_flying'),
                                  ('sharedassets32.assets', 63, 'zombie_five_footstep'),
                                  ('sharedassets57.assets', 11, 'small_hatcher_fly_loop'),
                                  ('sharedassets57.assets', 12, 'hatcher_fly_loop'),
                                  ('sharedassets9.assets', 52, 'toll_machine_activate'),
                                  ('sharedassets79.assets', 5, 'ruins_big_lift_activate'),
                                  ('sharedassets79.assets', 6, 'ruins_big_lift_arrive'),
                                  ('sharedassets79.assets', 7, 'ruins_big_lift_gate_door_thingy_close'),
                                  ('sharedassets7.assets', 112, 'ruins_big_lift_gate_door_thingy_open'),
                                  ('sharedassets7.assets', 108, 'manhole_key_turn'),
                                  ('sharedassets7.assets', 107, 'jiji_door_open_pt_1'),
                                  ('sharedassets7.assets', 98, 'jiji_door_open_pt_2')]:
        env = UnityPy.load(str(data/bundle))
        clip = next(obj for obj in env.objects if obj.path_id == path_id).read()
        stereo = name in {'toll_machine_activate', 'ruins_big_lift_activate', 'ruins_big_lift_arrive',
                          'jiji_door_open_pt_1', 'jiji_door_open_pt_2'}
        if clip.m_Name != name or clip.m_Channels != (2 if stereo else 1) or clip.m_Frequency != 44100:
            raise ValueError(f'Original clip identity changed: {bundle}:{path_id}')
        samples = clip.samples
        if set(samples) != {name+'.wav'}:
            raise ValueError(f'Unexpected decoded samples: {bundle}:{path_id}')
        prefix = 'sfx-enemy-' if bundle in {'sharedassets37.assets', 'sharedassets33.assets',
                                            'sharedassets32.assets', 'sharedassets57.assets'} else ''
        (output/f'{prefix}{name}.wav').write_bytes(samples[name+'.wav'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    extract(args.source, args.out)
