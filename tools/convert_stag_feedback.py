"""Admit reachable RestingGrounds_09 feedback; keep original pixels and PCM."""
import hashlib
import json
from pathlib import Path
import wave

from assetripper_client import ROOT
from convert_city_crest_effects import compile_data
from inspect_fsm import decode_fsm
from private_output_paths import is_private_output

SOURCE = ROOT / '_private/minimum-obstacles/resting-station-feedback'
ART = ROOT / '_private/minimum-obstacles/resting-station-art'


def require(ok, message):
    if not ok:
        raise ValueError(message)


def admit(source=SOURCE, art=ART):
    fsms = {i: decode_fsm(json.loads((art / f'level244-{i}.json').read_text())['m_Structure']['fsm']) for i in (1693, 1842)}
    states = {s['name']: s for s in fsms[1842]['states']}
    require([(t['fsmEvent']['name'], t['toState']) for t in states['Pause Before Box Drop']['transitions']] == [('FINISHED', 'Open Grate')], 'Changed station box bypass')
    # Box Disappear Anim is unreachable on this free lever station's OPEN path.
    require(not any(t['toState'] == 'Box Disappear Anim' for s in states.values() for t in s['transitions']), 'Station box feedback became reachable')
    for ident, state, role, ref in ((1693, 'Hit', 'hit', (4, 106)), (1842, 'Bell Up', 'bell', (6, 54))):
        rows = [a['parameters'] for s in fsms[ident]['states'] if s['name'] == state for a in s['actions']
                if a['enabled'] and a['type'].endswith('.AudioPlayerOneShotSingle')]
        require(len(rows) == 1, 'Missing station audio action')
        p = rows[0]
        require(p['audioClip']['value'] == dict(m_FileID=ref[0], m_PathID=ref[1]) and
                p['audioPlayer']['value'] == dict(m_FileID=2, m_PathID=4126) and
                p['spawnPoint']['name'] == 'Self' and all(not p[k]['useVariable'] and p[k]['value'] == v
                for k, v in (('pitchMin', 1), ('pitchMax', 1), ('volume', 1), ('delay', 0))), 'Changed station audio outcome')
    particles = [a['parameters'] for a in states['Bell Up']['actions'] if a['enabled'] and a['type'].endswith('.PlayParticleEmitter')]
    require(len(particles) == 1 and particles[0]['gameObject']['gameObject']['name'] == 'Dust Bell' and particles[0]['emit']['value'] == 0, 'Changed station bell dust dispatch')
    report = json.loads((source / 'source-report.json').read_text())
    clips = {c['role']: c for c in report['clips']}
    for role, name, coll, ident in (('hit', 'switch_gate_switch', 'sharedassets7.assets', 106), ('bell', 'bell_rise', 'sharedassets9.assets', 54)):
        row = clips[role]
        raw = (source / (role+'.wav')).read_bytes()
        require((row['name'], row['collection'], row['path_id']) == (name, coll, ident) and
                hashlib.sha256(raw).hexdigest() == row['sha256'], 'Changed original station PCM')
        with wave.open(str(source / (role+'.wav'))) as wav:
            require((wav.getframerate(), wav.getnchannels(), wav.getsampwidth(), wav.getcomptype()) == (44100, 1, 2, 'NONE'), 'Changed native station PCM format')
    require(clips['bell']['frames'] * 2 == 86308, 'Changed measured station PCM budget')


def compile_dust(first_sprite, source=SOURCE):
    admit(source)
    data, sprites, clips, records = compile_data(source, station=True, first_sprite=first_sprite)
    return b'SDF1'+data[4:], sprites, clips, records


def write_audio(output, source=SOURCE):
    require(is_private_output(output), 'Station feedback output must remain private')
    admit(source)
    # The identical lever-hit clip is already resident as TownLeverHit.
    (output / 'stag-bell-rise.wav').write_bytes((source / 'bell.wav').read_bytes())
