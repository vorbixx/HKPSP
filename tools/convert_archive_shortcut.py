"""Compile the source Fungus3_47 one-way archive wall (level223, GO106)."""
import converted_files  # Install copy-on-write protection for standalone conversion.
import argparse
import json
from pathlib import Path

from inspect_fsm import decode_fsm
from unity_transform import project


def convert(source):
    actors = {a['path_id']: a for a in source['actors']}
    transforms = {c['path_id']: c['data'] for a in actors.values()
                  for c in a['components'] if c['type'] == 'Transform'}
    wall = actors[106]
    if wall['name'] != 'One Way Wall':
        raise ValueError('Unexpected archive shortcut owner')
    fsm = next(c['data']['fsm'] for c in wall['components'] if c['type'] == 'PlayMakerFSM')
    states = {s['name']: s for s in decode_fsm(fsm)['states']}

    def actions(name, kind):
        return [a['parameters'] for a in states[name]['actions']
                if a['enabled'] and a['type'].rsplit('.', 1)[-1] == kind]

    switch = actions('Hit', 'IntSwitch')[0]
    if ([p['value'] for p in switch['compareTo']['elements']] != [0, 1, 2] or
            switch['sendEvent']['elements'] != ['HIT 1', 'HIT 2', 'HIT 3'] or
            actions('Hit', 'BoolTest')[0]['boolVariable'] != {'variable': 'Hero Range'}):
        raise ValueError('Unsupported archive wall hit/range policy')
    waits = [actions(s, 'Wait')[0]['time']['value'] for s in ('Hit 1', 'Hit 2')]
    field = next(v['value'] for v in fsm['variables']['stringVariables']
                 if v['name'] == 'PlayerData Name')
    if waits[0] != waits[1] or field != 'oneWayArchive' or not actions('PlayerData', 'SetPlayerDataBool'):
        raise ValueError('Unsupported archive wall persistence/timing')
    if not any(actions(s, 'DestroyAllChildren') for s in states):
        raise ValueError('Missing archive wall child destruction')

    def transform(actor):
        return next(c for c in actor['components'] if c['type'] == 'Transform')

    def chain(actor):
        result, t = [], transform(actor)['data']
        while True:
            result.append({k: {('m_' + axis.upper()): value for axis, value in t[k].items()}
                           for k in ('m_LocalScale', 'm_LocalRotation', 'm_LocalPosition')})
            parent = t['m_Father']['m_PathID']
            if not parent:
                return result
            t = transforms[parent]

    def quad(actor):
        box = next(c['data'] for c in actor['components'] if c['type'] == 'BoxCollider2D')
        if not box['m_IsTrigger'] or not box['m_Enabled']:
            raise ValueError('Archive wall requires enabled source trigger')
        x, y = box['m_Offset']['x'], box['m_Offset']['y']
        w, h = box['m_Size']['x'] / 2, box['m_Size']['y'] / 2
        return [project((x + dx, y + dy, 0), chain(actor))[:2]
                for dx, dy in ((-w, -h), (w, -h), (w, h), (-w, h))]

    root = transform(wall)['path_id']
    children = []
    for actor in actors.values():
        t = transform(actor)['data']
        while t['m_Father']['m_PathID']:
            if t['m_Father']['m_PathID'] == root:
                children.append(actor)
                break
            t = transforms[t['m_Father']['m_PathID']]
    if actors[108] not in children or not children:
        raise ValueError('Missing archive wall descendants/range')
    colliders = sorted(c['path_id'] for a in children for c in a['components']
                       if c['type'].endswith('Collider2D'))
    return dict(wait=waits[0], attack=quad(wall), hero_range=quad(actors[108]),
                children=sorted(a['path_id'] for a in children), colliders=colliders)


def header(data):
    def number(v):
        return format(v, '.12g') + ('.0f' if float(v).is_integer() else 'f')
    out = ['// Generated from level223 GO106 by tools/convert_archive_shortcut.py.',
           '#pragma once', '#include "physics.hpp"', '#include <array>',
           'namespace hk3ds::archive_shortcut_data {',
           'inline constexpr const char* scene="Fungus3_47";',
           'inline constexpr const char* playerData="oneWayArchive";',
           f'inline constexpr float hitWait={number(data["wait"])};',
           'inline constexpr unsigned hits=3;']
    for name, key in (('attack', 'attack'), ('heroRange', 'hero_range')):
        points = ','.join('Vec2{' + ','.join(number(v) for v in p) + '}' for p in data[key])
        out.append(f'inline constexpr std::array<Vec2,4> {name}={{{{{points}}}}};')
    for name in ('children', 'colliders'):
        out.append(f'inline constexpr std::array<unsigned,{len(data[name])}> {name}='
                   + '{{' + ','.join(map(str, data[name])) + '}};')
    return '\n'.join(out + ['}', ''])


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=root/'_private/minimum-bosses/uumuu/level223-source.json')
    parser.add_argument('--output', type=Path, default=root/'include/archive_shortcut_data.hpp')
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    facts = convert(json.loads(args.source.read_text(encoding='utf-8')))
    text = header(facts)
    if args.check:
        if not args.output.exists() or args.output.read_text(encoding='utf-8') != text:
            raise SystemExit('Archive shortcut data is stale')
    else:
        args.output.write_text(text, encoding='utf-8')
    print(f'Archive shortcut: {len(facts["children"])} children, {len(facts["colliders"])} colliders; 3 hits, {facts["wait"]:g}s wait')


if __name__ == '__main__':
    main()
