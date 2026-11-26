"""Recover RestingGrounds_09 reveal feedback; run under validation-lock -Exclusive."""
import hashlib
import io
import json
from pathlib import Path
import sys
import wave

from assetripper_client import ROOT
from inspect_fsm import decode_fsm


def main():
    sys.dont_write_bytecode = True
    sys.path[:0] = [str(ROOT / '_private/tools/boss-python'), str(ROOT / '_private/tools/audio-python')]
    import UnityPy
    source = ROOT / '_private/original/Hollow Knight/hollow_knight_Data'
    art = ROOT / '_private/minimum-obstacles/resting-station-art'
    output = ROOT / '_private/minimum-obstacles/resting-station-feedback'
    output.mkdir(parents=True, exist_ok=True)
    files, recovered, objects, clips = {}, set(), [], []

    def file(name):
        if name not in files:
            files[name] = next(iter(UnityPy.load(str(source / name)).files.values()))
        return files[name]

    def resolve(name, ref):
        return (Path(str(file(name).externals[ref['m_FileID']-1].path)).name
                if ref['m_FileID'] else name, ref['m_PathID'])

    def recover(name, ident):
        if not ident or (name, ident) in recovered:
            return
        recovered.add((name, ident))
        obj = file(name).objects[ident]
        tree = obj.read_typetree(check_read=obj.type.name != 'MonoBehaviour')
        stem = f'{name.replace(".assets", "")}-{ident}'
        raw = json.dumps(tree, indent=2, default=lambda b: dict(byteLength=len(b))).encode()
        (output / (stem+'.json')).write_bytes(raw)
        objects.append(dict(file=name, id=ident, type=obj.type.name, sha256=hashlib.sha256(raw).hexdigest()))
        if obj.type.name == 'Transform':
            recover(*resolve(name, tree['m_Father']))
        elif obj.type.name == 'ParticleSystemRenderer':
            for ref in tree['m_Materials']:
                recover(*resolve(name, ref))
        elif obj.type.name == 'Material':
            recover(*resolve(name, tree['m_Shader']))
            for _, prop in tree['m_SavedProperties']['m_TexEnvs']:
                recover(*resolve(name, prop['m_Texture']))
        elif obj.type.name == 'Texture2D':
            obj.read().image.save(output / (stem+'.png'))

    scene = file('level244')
    children = []
    for owner, expected in ((442, 'Dust Box Down'), (445, 'Dust Bell Up')):
        go = scene.objects[owner].read_typetree()
        if go['m_Name'] != expected:
            raise ValueError('Changed station dust identity')
        components = []
        recover('level244', owner)
        for entry in go['m_Component']:
            ident = entry['component']['m_PathID']
            obj = scene.objects[ident]
            recover('level244', ident)
            components.append(dict(id=ident, type=obj.type.name))
        children.append(dict(game_object=owner, name=expected, components=components))
    for ident, state, role in ((1693, 'Hit', 'hit'), (1842, 'Box Disappear Anim', 'box'), (1842, 'Bell Up', 'bell')):
        fsm = decode_fsm(json.loads((art / f'level244-{ident}.json').read_text())['m_Structure']['fsm'])
        actions = [a['parameters'] for s in fsm['states'] if s['name'] == state for a in s['actions']
                   if a['enabled'] and a['type'].endswith('.AudioPlayerOneShotSingle')]
        if len(actions) != 1:
            raise ValueError('Changed station audio action')
        params = actions[0]
        name, clip_id = resolve('level244', params['audioClip']['value'])
        clip = file(name).objects[clip_id].read()
        samples = clip.samples
        if len(samples) != 1:
            raise ValueError('Unsupported station audio samples')
        raw = next(iter(samples.values()))
        with wave.open(io.BytesIO(raw)) as wav:
            fmt = dict(rate=wav.getframerate(), channels=wav.getnchannels(), frames=wav.getnframes(), width=wav.getsampwidth())
        (output / (role+'.wav')).write_bytes(raw)
        clips.append(dict(role=role, name=clip.m_Name, collection=name, path_id=clip_id,
                          parameters=params, sha256=hashlib.sha256(raw).hexdigest(), **fmt))
    report = dict(scene='RestingGrounds_09', children=children, dependencies=objects, clips=clips)
    (output / 'source-report.json').write_text(json.dumps(report, indent=2)+'\n')
    print('Recovered', len(objects), 'station feedback objects')
    print('Audio:', [(c['role'], c['name'], c['collection'], c['path_id'], c['frames']*c['channels']*c['width']) for c in clips])


if __name__ == '__main__':
    main()
