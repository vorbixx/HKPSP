"""Convert the original arena's multizap patterns and MegaJellyZap lifecycle."""
import converted_files  # Install copy-on-write protection before output writes.
import argparse
import json
import math
from pathlib import Path
import re


def one(values, name):
    if len(values) != 1:
        raise ValueError(f'Expected one {name}')
    return values[0]


def component(actor, kind):
    value = one([c['data'] for c in actor['components'] if c['type'] == kind], kind)
    return value.get('m_Structure', value)


def sequence(code):
    body = code.split('private IEnumerator MultiZapSequence()', 1)[1].split('\n\t}', 1)[0]
    waits = re.findall(r'yield return new WaitForSeconds\((.*?)\);', body)
    number = r'([0-9]+(?:\.[0-9]+)?)(?:f)?'
    if len(waits) != 4:
        raise ValueError('Unsupported multizap coroutine waits')
    initial = re.fullmatch(r'Random.Range\(' + number + r',\s*' + number + r'\)', waits[0])
    timed = [re.fullmatch(number, wait) for wait in waits[1:]]
    if not initial or not all(timed):
        raise ValueError('Unsupported multizap wait expressions')
    low, high = map(float, initial.groups())
    times = [float(value.group(1)) for value in timed]
    if low != 0 or high < low or any(t <= 0 for t in times):
        raise ValueError('Unsupported multizap durations')
    if re.findall(r'col.enabled = (true|false);', body) != ['false', 'true', 'false']:
        raise ValueError('Unsupported multizap collider sequence')
    ordered = ['col.enabled = false;', 'yield return new WaitForSeconds(',
               'anim.Play("Zap Antic")', 'animMesh.enabled = true;',
               'yield return new WaitForSeconds(', 'col.enabled = true;',
               'anim.Play("Zap")', 'yield return new WaitForSeconds(',
               'col.enabled = false;', 'yield return StartCoroutine(anim.PlayAnimWait("Zap End"));',
               'animMesh.enabled = false;', 'yield return new WaitForSeconds(',
               'base.gameObject.SetActive(value: false);']
    cursor = 0
    for token in ordered:
        index = body.find(token, cursor)
        if index < 0:
            raise ValueError(f'Unsupported multizap action order at {token}')
        cursor = index + len(token)
    if ('Random.Range(0, 2)' not in body or 'Random.Range(0f, 360f)' not in body or
            'Zap,\n\t\tMultiZap' not in code):
        raise ValueError('Unsupported multizap random pose/type')
    return dict(delayMax=high, anticWait=times[0], activeWait=times[1], endPause=times[2])


def convert(source, code):
    facts = sequence(code)
    transforms = {c['path_id']: (a, c['data']) for a in source['actors']
                  for c in a['components'] if c['type'] == 'Transform'}
    def world(path, visited=()):
        if path in visited:
            raise ValueError('Cyclic pattern transform')
        _, transform = transforms[path]
        if transform['m_LocalRotation'] != dict(x=0.0, y=0.0, z=0.0, w=1.0):
            raise ValueError('Unsupported rotated pattern root')
        p, s = transform['m_LocalPosition'], transform['m_LocalScale']
        parent = transform['m_Father']
        if parent['m_FileID']:
            raise ValueError('External pattern parent')
        if parent['m_PathID']:
            px, py, sx, sy = world(parent['m_PathID'], visited+(path,))
            return px+p['x']*sx, py+p['y']*sy, s['x']*sx, s['y']*sy
        return p['x'], p['y'], s['x'], s['y']
    root = one([a for a in source['actors'] if a['name'] == 'Mega Jellyfish Multizaps'], 'Multizaps')
    patterns = component(root, 'Transform')['m_Children']
    if len(patterns) != 2:
        raise ValueError('Unsupported pattern count')
    positions, offsets, profile = [], [0], None
    for pattern_ref in patterns:
        if pattern_ref['m_FileID']:
            raise ValueError('External pattern')
        pattern, transform = transforms[pattern_ref['m_PathID']]
        if pattern['name'] not in ('Pattern 1', 'Pattern 2') or not transform['m_Children']:
            raise ValueError('Unsupported pattern hierarchy')
        for child in transform['m_Children']:
            if child['m_FileID']:
                raise ValueError('External multizap actor')
            actor, _ = transforms[child['m_PathID']]
            zap, damage, circle = [component(actor, kind) for kind in ('MegaJellyZap', 'DamageHero', 'CircleCollider2D')]
            if (zap['type'] != 1 or damage['hazardType'] != 1 or damage['shadowDashHazard'] or
                    not circle['m_IsTrigger'] or circle['m_Offset'] != dict(x=0.0, y=0.0)):
                raise ValueError('Unsupported multizap damage contract')
            x, y, sx, sy = world(child['m_PathID'])
            if abs(sx) != 1 or abs(sy) != 1:
                raise ValueError('Unsupported scaled multizap collider')
            radius, amount = circle['m_Radius'], damage['damageDealt']
            if not all(math.isfinite(v) for v in (x,y,radius)) or radius <= 0 or type(amount) is not int or amount <= 0:
                raise ValueError('Invalid multizap damage profile')
            if profile is not None and profile != (radius, amount):
                raise ValueError('Mixed multizap profiles')
            profile = radius, amount
            positions.append((x, y))
        offsets.append(len(positions))
    return dict(**facts, radius=profile[0], damage=profile[1], offsets=offsets, positions=positions)


def header(facts):
    rows = ['// Generated from copied build22529139 level228 and MegaJellyZap.cs.',
            '#pragma once', '#include "physics.hpp"', 'namespace hk3ds::uumuu_multizap_data {']
    for name in ('delayMax', 'anticWait', 'activeWait', 'endPause', 'radius'):
        rows.append(f'inline constexpr float {name}={facts[name]:.9f}f;')
    rows += [f'inline constexpr unsigned damage={facts["damage"]};',
             f'inline constexpr unsigned count={len(facts["positions"])};',
             'inline constexpr unsigned offsets[]={'+','.join(map(str,facts['offsets']))+'};',
             'inline constexpr Vec2 positions[]={']
    rows += [f'    {{{x:.9f}f,{y:.9f}f}},' for x,y in facts['positions']]
    return '\n'.join(rows+['};','}',''])


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=root/'_private/minimum-bosses/uumuu/level228-source.json')
    parser.add_argument('--code', type=Path, default=root/'_private/research/decompiled/all/MegaJellyZap.cs')
    parser.add_argument('--output', type=Path, default=root/'include/uumuu_multizap_data.hpp')
    args = parser.parse_args()
    facts = convert(json.loads(args.source.read_text()), args.code.read_text())
    args.output.write_text(header(facts))
    print(json.dumps({'actors':len(facts['positions']), 'offsets':facts['offsets']}))


if __name__ == '__main__':
    main()
