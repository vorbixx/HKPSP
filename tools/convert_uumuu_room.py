"""Validate the archive's conditional Uumuu overlay and world-space anchors."""
import converted_files  # Protect this worktree's converted outputs.
import argparse
import json
import math
from pathlib import Path

from convert_uumuu_quirrel import actor, comp, header


def root_position(source, name):
    obj = actor(source, name)
    if not obj['active']:
        raise ValueError(f'{name} must be active')
    transform = comp(obj, 'Transform')['data']
    if (transform['m_Father'] != {'m_FileID': 0, 'm_PathID': 0} or
            transform['m_LocalRotation'] != dict(x=0, y=0, z=0, w=1) or
            transform['m_LocalScale'] != dict(x=1, y=1, z=1)):
        raise ValueError(f'Unsupported {name} parent/rotation/scale')
    position = transform['m_LocalPosition']
    if any(not math.isfinite(v) for v in position.values()):
        raise ValueError(f'Invalid {name} position')
    return position


def collider_facts(obj, kind, geometry):
    data = comp(obj, kind)['data']
    if data.get('m_Enabled') != 0 or data.get('m_IsTrigger') is not True:
        raise ValueError(f'Unsupported Mega Jellyfish {kind} activation/trigger')
    offset = data['m_Offset']
    if any(not math.isfinite(v) for v in offset.values()):
        raise ValueError(f'Invalid Mega Jellyfish {kind} offset')
    prefix = 'boxCollider' if geometry == 'box' else 'circleCollider'
    facts = {f'{prefix}Enabled': False, f'{prefix}IsTrigger': True,
             f'{prefix}OffsetX': offset['x'], f'{prefix}OffsetY': offset['y']}
    if geometry == 'box':
        size = data['m_Size']
        if any(not math.isfinite(v) or v <= 0 for v in size.values()):
            raise ValueError('Invalid Mega Jellyfish BoxCollider2D size')
        facts.update(boxSizeX=size['x'], boxSizeY=size['y'])
    else:
        radius = data['m_Radius']
        if not math.isfinite(radius) or radius <= 0:
            raise ValueError('Invalid Mega Jellyfish CircleCollider2D radius')
        facts['circleRadius'] = radius
    return facts


def body_facts(obj):
    body = comp(obj, 'Rigidbody2D')['data']
    expected = dict(m_BodyType=0, m_Simulated=True, m_UseFullKinematicContacts=False,
                    m_UseAutoMass=False, m_Mass=1.0, m_LinearDamping=0.0,
                    m_AngularDamping=0.05000000074505806, m_GravityScale=0.0,
                    m_Interpolate=0, m_SleepingMode=0, m_CollisionDetection=1,
                    m_Constraints=4)
    if any(body.get(key) != value for key, value in expected.items()):
        raise ValueError('Unsupported Mega Jellyfish Rigidbody2D configuration')
    return dict(bodyType=body['m_BodyType'], bodySimulated=body['m_Simulated'],
                bodyUseFullKinematicContacts=body['m_UseFullKinematicContacts'],
                bodyUseAutoMass=body['m_UseAutoMass'], bodyMass=body['m_Mass'],
                bodyLinearDamping=body['m_LinearDamping'],
                bodyAngularDamping=body['m_AngularDamping'],
                bodyGravityScale=body['m_GravityScale'], bodyInterpolate=body['m_Interpolate'],
                bodySleepingMode=body['m_SleepingMode'],
                bodyCollisionDetection=body['m_CollisionDetection'],
                bodyConstraints=body['m_Constraints'])


def damage_facts(obj):
    damage = comp(obj, 'DamageHero')['data']
    expected = dict(m_Enabled=1, damageDealt=1, hazardType=1,
                    shadowDashHazard=0, resetOnEnable=0)
    if any(damage.get(key) != value for key, value in expected.items()):
        raise ValueError('Unsupported Mega Jellyfish DamageHero configuration')
    return dict(damageEnabled=True, damage=damage['damageDealt'],
                hazardType=damage['hazardType'], shadowDashHazard=bool(damage['shadowDashHazard']),
                damageResetOnEnable=bool(damage['resetOnEnable']))


def convert(room, overlay, overlay_objects):
    for source, scene in ((room, 'level228'), (overlay, 'level229')):
        if source.get('build_id') != 22529139 or source.get('scene') != scene:
            raise ValueError(f'Expected build22529139 {scene}')
    room_hashes = {k.replace('\\', '/'): v for k, v in room['source_sha256'].items()}
    overlay_hashes = {k.replace('\\', '/'): v for k, v in overlay['source_sha256'].items()}
    common = room_hashes.keys() & overlay_hashes.keys()
    if ('Managed/Assembly-CSharp.dll' not in common or
            any(room_hashes[k] != overlay_hashes[k] for k in common)):
        raise ValueError('Room and overlay must come from the same copied original')
    root_position(room, 'BossLoader')
    loader = comp(actor(room, 'BossLoader'), 'SceneAdditiveLoadConditional')['data']
    loader = loader.get('m_Structure', loader)
    required = dict(m_Enabled=1, sceneNameToLoad='Fungus3_archive_02_boss',
                    altSceneNameToLoad='', needsPlayerDataBool='defeatedMegaJelly',
                    playerDataBoolValue=0, needsPlayerDataInt='', extraBoolTests=[],
                    extraIntTests=[], usePersistentBoolItem=0, doorTrigger='')
    if any(loader.get(k) != v for k, v in required.items()):
        raise ValueError('Unsupported archive additive-load condition')
    if any(loader['saveStateData'][k] for k in ('id', 'sceneName')):
        raise ValueError('Unsupported loader persistence callback')
    if any(x['name'] in ('Mega Jellyfish', 'Battle Scene') for x in room['actors']):
        raise ValueError('Boss actors must belong to the additive overlay')
    boss = root_position(overlay, 'Mega Jellyfish')
    jelly = actor(overlay, 'Mega Jellyfish')
    source_object = overlay_objects.get(str(jelly['path_id']))
    if (not source_object or source_object.get('m_Name') != 'Mega Jellyfish' or
            type(source_object.get('m_Layer')) is not int or
            not 0 <= source_object['m_Layer'] <= 31):
        raise ValueError('Missing or invalid Mega Jellyfish source layer')
    boss_layer = source_object['m_Layer']
    deactivation = comp(jelly, 'DeactivateIfPlayerdataTrue')['data']
    if not deactivation['m_Enabled'] or deactivation['boolName'] != loader['needsPlayerDataBool']:
        raise ValueError('Boss deactivation must use the loader source bool')
    body = body_facts(jelly)
    box = collider_facts(jelly, 'BoxCollider2D', 'box')
    circle = collider_facts(jelly, 'CircleCollider2D', 'circle')
    damage = damage_facts(jelly)
    multizap = root_position(room, 'Mega Jellyfish Multizaps')
    battle = root_position(overlay, 'Battle Scene')
    collider = comp(actor(overlay, 'Battle Scene'), 'BoxCollider2D')['data']
    if not collider['m_Enabled'] or not collider['m_IsTrigger']:
        raise ValueError('Battle admission must be an enabled trigger')
    size, offset = collider['m_Size'], collider['m_Offset']
    if (any(not math.isfinite(v) for v in (*size.values(), *offset.values())) or
            size['x'] <= 0 or size['y'] <= 0):
        raise ValueError('Invalid battle trigger')
    x, y = battle['x'] + offset['x'], battle['y'] + offset['y']
    return dict(roomName='Fungus3_archive_02', overlayName=loader['sceneNameToLoad'],
                playerDataBool=loader['needsPlayerDataBool'], loadWhenDefeated=False,
                bossX=boss['x'], bossY=boss['y'], bossLayer=boss_layer,
                multizapX=multizap['x'], multizapY=multizap['y'],
                triggerLeft=x-size['x']/2, triggerBottom=y-size['y']/2,
                triggerRight=x+size['x']/2, triggerTop=y+size['y']/2,
                **body, **box, **circle, **damage)


def room_header(facts):
    return header(facts).replace(
        'scene level229 by tools/convert_uumuu_quirrel.py',
        'scenes level228/229 by tools/convert_uumuu_room.py').replace(
        'hk3ds::uumuu_quirrel_data', 'hk3ds::uumuu_room_data')


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--room', type=Path, default=root/'_private/minimum-bosses/uumuu/level228-source.json')
    parser.add_argument('--overlay', type=Path, default=root/'_private/minimum-bosses/uumuu/level229-source.json')
    parser.add_argument('--objects', type=Path, default=root/'_private/minimum-bosses/uumuu/level229-objects.json')
    parser.add_argument('--output', type=Path, default=root/'include/uumuu_room_data.hpp')
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    facts = convert(json.loads(args.room.read_text(encoding='utf-8')),
                    json.loads(args.overlay.read_text(encoding='utf-8')),
                    json.loads(args.objects.read_text(encoding='utf-8')))
    text = room_header(facts)
    if args.check:
        if not args.output.exists() or args.output.read_text(encoding='utf-8') != text:
            raise SystemExit('generated Uumuu room metadata is stale')
    else:
        args.output.write_text(text, encoding='utf-8')
    print(json.dumps(facts, sort_keys=True))


if __name__ == '__main__':
    main()
