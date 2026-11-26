"""Derive Broken Vessel encounter data from the private original scene export."""
import converted_files
import argparse
import json
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def convert(source, physics=None):
    data = json.loads(source.read_text())
    if data['room'] != 'Abyss_19' or data['scene'] != 'level343':
        raise ValueError('Expected the original Abyss_19 export')
    actors = {a['path_id']: a for a in data['actors']}

    def component(actor, kind):
        values = [c for c in actor['components'] if c['type'] == kind]
        if len(values) != 1:
            raise ValueError(f'Expected one {kind} on {actor["name"]}')
        return values[0]

    def fsm(actor, name):
        return next(c['decoded'] for c in actor['components']
                    if c.get('decoded', {}).get('name') == name)

    def literal(value):
        if isinstance(value, dict):
            if value.get('useVariable') or 'variable' in value:
                raise ValueError(f'Expected literal: {value}')
            return value['value']
        return value

    def action(control, state, kind, field):
        selected = next(s for s in control['states'] if s['name'] == state)
        matches = [a for a in selected['actions'] if a['enabled'] and a['type'].split('.')[-1] == kind]
        if len(matches) != 1:
            raise ValueError(f'Expected one {kind} in {state}')
        return literal(matches[0]['parameters'][field])

    def world(transform):
        t = data['transforms'][str(transform)]
        p, s, q = t['m_LocalPosition'], t['m_LocalScale'], t['m_LocalRotation']
        if abs(q['x']) > 1e-5 or abs(q['y']) > 1e-5:
            raise ValueError('Encounter geometry must remain in the XY plane')
        angle = 2 * math.atan2(q['z'], q['w'])
        c, v = math.cos(angle), math.sin(angle)
        local = (c*s['x'], -v*s['y'], v*s['x'], c*s['y'], p['x'], p['y'])
        parent = t['m_Father']['m_PathID']
        if not parent:
            return local
        a,b,c,d,x,y = world(parent)
        e,f,g,h,u,v = local
        return a*e+b*g, a*f+b*h, c*e+d*g, c*f+d*h, a*u+b*v+x, c*u+d*v+y

    def box(actor, local=False):
        collider = component(actor, 'BoxCollider2D')['data']
        matrix = (1,0,0,1,0,0) if local else world(component(actor,'Transform')['path_id'])
        a,b,c,d,x,y = matrix
        o,z = collider['m_Offset'],collider['m_Size']
        points = [(a*u+b*v+x,c*u+d*v+y)
                  for u in (o['x']-z['x']/2,o['x']+z['x']/2)
                  for v in (o['y']-z['y']/2,o['y']+z['y']/2)]
        return min(p[0] for p in points),min(p[1] for p in points),max(p[0] for p in points),max(p[1] for p in points)

    boss = actors[78]
    control = fsm(boss, 'IK Control')
    floats = {v['name']:v['value'] for v in control['variables']['floatVariables']}
    corpse = next(a for a in data['prefabs'] if a['name'] == 'Corpse Infected Knight')
    death = fsm(corpse, 'corpse')
    clips = component(boss, 'tk2dSpriteAnimator')['animation_library']['clips']
    stun = fsm(boss, 'Stun Control')
    stun_ints = {v['name']:v['value'] for v in stun['variables']['intVariables']}
    stun_floats = {v['name']:v['value'] for v in stun['variables']['floatVariables']}
    physics = physics or ROOT/'_private/research/assets/Physics2DSettings.json'
    gravity = -json.loads(physics.read_text())['m_Gravity']['m_Y']
    constants = dict(gravity=gravity, rumbleTime=action(control,'Rumble Start','Wait','time'),
                     wake1Time=action(control,'Spawning 1','Wait','time'),
                     wake2Time=action(control,'Spawning 2','Wait','time'),
                     wake3Time=action(control,'Spawning 3','Wait','time'),
                     stunHits=stun_ints['Stun Hit Max'],stunCombo=stun_ints['Stun Combo'],
                     stunsMax=stun_ints['Stuns Max'],comboTime=stun_floats['Combo Time'],
                     rewakeX=action(control,'Set Pos','SetPosition','x'),
                     rewakeY=action(control,'Set Pos','SetPosition','y'),
                     hp=component(boss,'HealthManager')['data']['hp'],
                     dashSpeed=floats['Dash Speed'], evadeSpeed=floats['Evade Speed'],
                     gravityScale=floats['Gravity'], airDashHeight=floats['Air Dash Height'],
                     downstabHeight=floats['Min Dstab Height'],
                     leftX=floats['Left X'], rightX=floats['Right X'],
                     jumpSpeed=action(control,'Jump','RandomFloat','min'),
                     roarTime=action(control,'Roar','Wait','time'),
                     startPause=action(control,'Start Pause','Wait','time'),
                     stunTime=action(control,'Stunned','Wait','time'),
                     downstabRecover=action(control,'Dstab Recover','Wait','time'),
                     corpsePause=action(death,'Pause','Wait','time'),
                     corpseSteam=action(death,'Steam','Wait','time'),
                     corpseReady=action(death,'Ready','Wait','time'),
                     corpseBlow=action(death,'Blow','Wait','time'))
    splash_actor = next(a for a in data['prefabs'] if a['name'] == 'Spit Effect')
    splash = component(splash_actor, 'Transform')['data']
    splash_sprite = component(splash_actor, 'tk2dSprite')['data']['_scale']
    scuttler = fsm(next(a for a in data['prefabs'] if a['name'] == 'Orange Scuttler IK'), 'Orange Scuttler')
    q = splash['m_LocalRotation']
    constants.update(splashOffsetX=splash['m_LocalPosition']['x'],
                     splashOffsetY=splash['m_LocalPosition']['y'],
                     splashScaleX=splash['m_LocalScale']['x']*splash_sprite['x'],
                     splashScaleY=splash['m_LocalScale']['y']*splash_sprite['y'],
                     splashRotation=2*math.atan2(q['z'],q['w']),
                     splashAngleMin=math.radians(action(scuttler,'Splut','RandomFloat','min')),
                     splashAngleMax=math.radians(action(scuttler,'Splut','RandomFloat','max')),
                     splashTime=action(scuttler,'Splut','Wait','time'))
    def number(v):
        text = format(float(v), '.9g')
        return text+('f' if '.' in text or 'e' in text else '.0f')
    lines=['#pragma once','#include "physics.hpp"','#include <cstring>',
           '// Generated by tools/convert_broken_vessel.py; original build '+str(data['build_id'])+'.',
           'namespace hk3ds { namespace broken_vessel_data {']
    lines += [f'inline constexpr float {k}={number(v)};' for k,v in constants.items()]
    lines += ['inline constexpr Rect body={'+','.join(map(number,box(boss,True)))+'};']
    _,_,_,_,x,y=world(component(boss,'Transform')['path_id'])
    lines += ['inline constexpr Vec2 spawn={'+number(x)+','+number(y)+'};']
    for key,name in [('trigger','Battle Start'),('gate','Battle Gate'),('rewake','Rewake Range')]:
        a = next(a for a in actors.values() if a['name']==name)
        lines += ['inline constexpr Rect '+key+'={'+','.join(map(number,box(a)))+'};']
    for key,name in [('evadeRange','Evade Range'),('evadeCheck','Evade Check'),('dashRange','Dash Range'),('overheadRange','Ohead Range'),('overheadHit','Overhead Slash')]:
        r=box(next(a for a in actors.values() if a['name']==name))
        r=(r[0]-x,r[1]-y,r[2]-x,r[3]-y)
        lines += ['inline constexpr Rect '+key+'={'+','.join(map(number,r))+'};']
    slash=next(a for a in actors.values() if a['name']=='Dash Slash Hit')
    polygon=component(slash,'PolygonCollider2D')['data']
    a,b,c,d,u,v=world(component(slash,'Transform')['path_id'])
    points=polygon['m_Points']['m_Paths'][0]
    lines += ['inline constexpr Vec2 dashHit[]={'+','.join('{'+number(a*p['x']+b*p['y']+u-x)+','+number(c*p['x']+d*p['y']+v-y)+'}' for p in points)+'};']
    def subtree(root):
        transform = component(root, 'Transform')['path_id']
        def inside(actor):
            current = component(actor, 'Transform')['path_id']
            while current:
                if current == transform:
                    return True
                current = data['transforms'][str(current)]['m_Father']['m_PathID']
            return False
        return [a for a in actors.values() if inside(a)]
    for prefix, name in [('gate', 'Battle Gate'), ('door', 'infected_door'), ('boss', 'Infected Knight')]:
        group = subtree(next(a for a in actors.values() if a['name'] == name))
        ids = [a['path_id'] for a in group]
        colliders = [c['path_id'] for a in group for c in a['components'] if c['type'] in ('BoxCollider2D', 'PolygonCollider2D', 'CircleCollider2D')]
        lines += ['inline constexpr unsigned '+prefix+'Owners[]={'+','.join(map(str,ids))+'};']
        if colliders:
            lines += ['inline constexpr unsigned '+prefix+'Colliders[]={'+','.join(map(str,colliders))+'};']
    lines += ['template<unsigned N> inline bool contains(const unsigned (&ids)[N], unsigned id){for(auto value:ids)if(value==id)return true;return false;}']
    lines += ['inline float clipTime(const char* name) {']
    for c in clips:
        if c['name'] and c['frames']:
            lines += ['    if(!std::strcmp(name,'+json.dumps(c['name'])+'))return '+number(len(c['frames'])/c['fps'])+';']
    lines += ['    return 0;','}','} }','']
    return '\n'.join(lines)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,default=ROOT/'_private/minimum-bosses/broken-vessel/level343-source.json')
    parser.add_argument('--physics',type=Path,default=ROOT/'_private/research/assets/Physics2DSettings.json')
    parser.add_argument('--output',type=Path,default=ROOT/'include/broken_vessel_data.hpp')
    args=parser.parse_args()
    args.output.write_text(convert(args.source,args.physics),encoding='utf-8',newline='\n')  # LF: CRLF dirtied main after every regen and blocked safe-merge
    print('Generated',args.output)


if __name__=='__main__':
    main()
