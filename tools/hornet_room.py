"""Merge the original additive Protector actor into the playable Greenpath room."""
import json
from assetripper_client import ROOT
from convert_room_visuals import additive_owner_id
from dung_defender_room import arena_floor


def compile_room(report):
    rows = json.loads((ROOT/'_private/research/hornet-protector/scene.json').read_text())
    boss = next(r for r in rows if r['name'] == 'Hornet Boss 1')
    encounter = next(r for r in rows if r['name'] == 'Hornet Infected Knight Encounter')

    def component(row, kind):
        return next(c['data'] for c in row['components'] if c['type'] == kind)

    transform = component(boss, 'Transform')
    position = transform['m_LocalPosition']
    constrain = component(boss, 'ConstrainPosition')
    intro = component(encounter, 'Transform')['m_LocalPosition']
    box = component(encounter, 'BoxCollider2D')
    center = [intro[a]+box['m_Offset'][a] for a in ('x','y')]
    half = [box['m_Size'][a]/2 for a in ('x','y')]
    trigger = [center[0]-half[0], center[1]-half[1], center[0]+half[0], center[1]+half[1]]
    floor = arena_floor(report['shapes'], position['x'], position['y'])
    arena = [constrain['xMin'], floor, constrain['xMax'], trigger[3]]
    spawn = dict(owner=additive_owner_id(boss['path_id']), prefab=boss['name'],
                 position=[position['x'],position['y']], scale=[1,1], active=True)
    return spawn, dict(arena=arena, trigger=trigger, intro=[intro['x'],intro['y']])
