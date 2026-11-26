"""Convert the pooled MegaJellyZap prefab and its C# coroutine into logic data."""
import converted_files  # Install copy-on-write protection before output writes.
import argparse
import json
import math
from pathlib import Path
import re


def component(actor, kind):
    values = [c['data'] for c in actor['components'] if c['type'] == kind]
    if len(values) != 1:
        raise ValueError(f'Expected one {kind}')
    return values[0].get('m_Structure', values[0])


def convert(source, code):
    if source['build_id'] != 22529139 or source['source_reference'] != 'level229 external3 path28':
        raise ValueError('Unsupported pooled zap source')
    roots = [a for a in source['actors'] if a['name'] == 'Mega Jelly Zap']
    if len(roots) != 1:
        raise ValueError('Expected one pooled zap')
    root = roots[0]
    script = component(root, 'MegaJellyZap')
    damage = component(root, 'DamageHero')
    circle = component(root, 'CircleCollider2D')
    transform = component(root, 'Transform')
    if script['type'] != 0 or 'Zap,\n\t\tMultiZap' not in code:
        raise ValueError('Expected MegaJellyZap.Type.Zap')
    if damage['damageDealt'] != 1 or damage['hazardType'] != 1 or damage['shadowDashHazard']:
        raise ValueError('Unsupported pooled zap damage')
    if not circle['m_IsTrigger'] or circle['m_Offset'] != dict(x=0.0,y=0.0):
        raise ValueError('Unsupported pooled zap collider')
    radius = circle['m_Radius']
    if not math.isfinite(radius) or radius <= 0:
        raise ValueError('Invalid zap radius')
    if transform['m_LocalScale'] != dict(x=1.0,y=1.0,z=1.0):
        raise ValueError('Unsupported pooled zap scale')
    body = code.split('private IEnumerator ZapSequence()',1)[1].split('\n\t}',1)[0]
    waits = re.findall(r'yield return new WaitForSeconds\(([0-9]+(?:\.[0-9]+)?)f?\);',body)
    if len(waits) != 3 or body.count('yield return') != 3:
        raise ValueError('Unsupported pooled zap waits')
    times = list(map(float,waits))
    if any(t <= 0 for t in times):
        raise ValueError('Invalid zap duration')
    ordered = ['col.enabled = false;', 'ptAttack.Stop();', 'ptAntic.Play();',
               'fade.Fade(up: true);', 'yield return new WaitForSeconds(',
               'zapBugPt1.SpawnAndPlayOneShot(', 'col.enabled = true;', 'ptAttack.Play();',
               'yield return new WaitForSeconds(', 'zapBugPt2.SpawnAndPlayOneShot(',
               'fade.Fade(up: false);', 'ptAttack.Stop();', 'ptAntic.Stop();',
               'col.enabled = false;', 'yield return new WaitForSeconds(', 'base.gameObject.Recycle();']
    cursor = 0
    for token in ordered:
        index = body.find(token,cursor)
        if index < 0:
            raise ValueError(f'Unsupported pooled zap action order at {token}')
        cursor = index + len(token)
    if re.findall(r'col.enabled = (true|false);',body) != ['false','true','false']:
        raise ValueError('Unsupported zap damage windows')
    return dict(anticWait=times[0],activeWait=times[1],endPause=times[2],radius=radius,damage=damage['damageDealt'])


def header(facts):
    rows = ['// Generated from build22529139 level229 external3/path28 and MegaJellyZap.cs.',
            '#pragma once','namespace hk3ds::uumuu_zap_data {']
    for name in ('anticWait','activeWait','endPause','radius'):
        rows.append(f'inline constexpr float {name}={facts[name]:.9f}f;')
    return '\n'.join(rows+[f'inline constexpr unsigned damage={facts["damage"]};','}',''])


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,default=root/'_private/minimum-bosses/uumuu/pooled-zap-source.json')
    parser.add_argument('--code',type=Path,default=root/'_private/research/decompiled/all/MegaJellyZap.cs')
    parser.add_argument('--output',type=Path,default=root/'include/uumuu_zap_data.hpp')
    args = parser.parse_args()
    facts = convert(json.loads(args.source.read_text()),args.code.read_text())
    args.output.write_text(header(facts))
    print(json.dumps(facts))


if __name__ == '__main__':
    main()
