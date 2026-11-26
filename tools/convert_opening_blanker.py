"""Generate the new-game overlay from the enabled source CameraFade actions."""
import converted_files  # Install hardlink copy-on-write before asset writes.
import hashlib
import json
import re
from pathlib import Path
from inspect_fsm import decode_fsm

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / '_private/research/opening-blanker'


def convert(source=SOURCE, header=ROOT / 'include/opening_blanker_data.hpp'):
    mapping = json.loads((source / 'source-map.json').read_text())
    for name, expected in mapping['source_sha256'].items():
        if hashlib.sha256((source / name).read_bytes()).hexdigest() != expected:
            raise ValueError('Opening blanker source hash mismatch: ' + name)
    camera = decode_fsm(json.loads((source / mapping['camera']).read_text())['m_Structure']['fsm'])
    blanker = decode_fsm(json.loads((source / mapping['blanker']).read_text())['m_Structure']['fsm'])

    def action(fsm, state, kind):
        state = next(s for s in fsm['states'] if s['name'] == state)
        return next(a['parameters'] for a in state['actions'] if a['enabled'] and a['type'].endswith('.' + kind))

    fade = action(camera, 'FadeIn', 'SetFsmFloat')['setValue']['value']
    delay = action(camera, 'FadeIn', 'SendEventByName')['delay']['value']
    action(camera, 'Pre Fade', 'NextFrameEvent')
    ease = action(blanker, 'Fade Out', 'EaseColor')
    if ease['time'] != {'variable': 'Fade Time'} or not ease['realTime'] or ease['easeType'] != 21:
        raise ValueError('Unexpected source fade clock')
    colours = {v['name']: v['value'] for v in blanker['variables']['colorVariables']}
    start, end = colours['End Colour'], colours['Start Colour']
    if start != dict(r=0, g=0, b=0, a=1) or end != dict(r=0, g=0, b=0, a=0):
        raise ValueError('Unexpected source blanker colours')
    hero = (source / 'HeroController.cs').read_text()
    wait = float(re.search(r'yield return new WaitForSeconds\((0\.165)f\);', hero)[1])
    generated = ('// Generated from Build 22529139 by tools/convert_opening_blanker.py.\n'
                 '#pragma once\nnamespace hk3ds::openingdata {\n'
                 f'inline constexpr float entryWait={wait:.9f}f,fadeDelay={delay:.9f}f,fadeTime={fade:.9f}f;\n'
                 'inline constexpr float red=0,green=0,blue=0;\n}\n')
    header.write_text(generated, encoding='utf-8', newline='\n')
    return dict(entry_wait=wait, fade_delay=delay, fade_time=fade, colour=start,
                source_sha256=mapping['source_sha256'])


if __name__ == '__main__':
    print(json.dumps(convert()))
