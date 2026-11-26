"""Private plant sprite admission and index bindings; no runtime clock policy."""
import hashlib
import json
import math
import struct

from analyze_plants import observe, require


def digest(data):
    return hashlib.sha256(data).hexdigest()


def sprite_key(clip, key):
    require(key['m_Value']['m_FileID'] == 0, 'External plant sprite unresolved')
    return f"{clip.rsplit('-', 1)[0]}-{key['m_Value']['m_PathID']}.json"


def passive_plants(profile, manifest, room, mapping):
    """The three scenery-only plants share the validated interactive clips."""
    def read(name):
        raw = (room / name).read_bytes()
        require(digest(raw) == mapping['source_sha256'][name], 'Changed passive plant: ' + name)
        return json.loads(raw)

    controllers = {}
    for plant in manifest['plants']:
        animator = json.loads((room / plant['animator']).read_bytes())
        ref = animator['m_Controller']
        controllers[(ref['m_FileID'], ref['m_PathID'])] = next(
            p for p in profile['plants'] if p['owner'] == plant['owner'])
    known = {p['owner'] for p in profile['plants']}
    result = {}
    for row in mapping['inventory']:
        owner = int(row['go'][7:-5])
        if not row['active'] or owner in known or owner in result:
            continue
        go = read(row['go'])
        if not go['m_Name'].startswith('health plant '):
            continue
        components = [read('level6-' + str(c['m_Component']['m_PathID']) + '.json')
                      for c in go['m_Components']]
        animators = [c for c in components if 'm_Controller' in c and 'm_CullingMode' in c]
        if not animators:
            continue
        require(len(animators) == 1 and not any('m_Structure' in c and
                'debrisParts' in c['m_Structure'] for c in components),
                'Passive plant acquired gameplay behavior')
        animator = animators[0]
        require(animator['m_Enabled'] and animator['m_CullingMode'] == 2 and
                animator['m_UpdateMode'] == 0 and not animator['m_ApplyRootMotion'],
                'Unsupported passive plant Animator')
        ref = animator['m_Controller']
        exemplar = controllers[(ref['m_FileID'], ref['m_PathID'])]
        helpers = [c['m_Structure'] for c in components if 'stateToPlay' in c.get('m_Structure', {})]
        require(len(helpers) <= 1 and all(h['stateToPlay'] == exemplar['state']['name'] and
                not h['onEnable'] for h in helpers), 'Unsupported passive random start')
        result[owner] = dict(owner=owner, clip=exemplar['clip'], state=exemplar['state'],
                             random_start=bool(helpers))
    require(set(result) == {854, 2566, 3177}, 'Changed passive plant inventory')
    return [result[k] for k in sorted(result)]


def sources(plant_source, room_source, mapping):
    """Revalidate saved dependencies and return only additional artwork sources."""
    profile = observe(plant_source, room_source)
    manifest = json.loads((plant_source / 'manifest.json').read_bytes())
    profile['passive_plants'] = passive_plants(profile, manifest, room_source, mapping)
    sprites = {sprite_key(name, key) for name, clip in profile['clips'].items()
               for key in clip['keys']}
    extra = {}
    for name in sorted(sprites):
        sprite = json.loads((plant_source / name).read_bytes())
        collection = name.rsplit('-', 1)[0]
        texture = f"{collection}-{sprite['m_RD']['m_Texture']['m_PathID']}.json"
        image = manifest['textures'][texture]
        if name in mapping['sprites']:
            require(digest((room_source / name).read_bytes()) == manifest['source_sha256'][name],
                    'Conflicting shared plant sprite: ' + name)
            require(digest((room_source / mapping['sprites'][name]).read_bytes()) ==
                    manifest['source_sha256'][image], 'Conflicting shared plant texture: ' + name)
        else:
            extra[name] = (plant_source / image, sprite)
    return profile, extra


def bind(profile, mapping, by_sprite, draws, binary, room_manifest):
    """Keep exact key times/holds and require single-tile sprite replacement."""
    def index(name):
        indices = by_sprite.get(name, [])
        require(len(indices) == 1, 'Plant sprite must occupy exactly one tile: ' + name)
        return indices[0]

    clips = {}
    for name, clip in profile['clips'].items():
        clips[name] = dict(duration=clip['duration'], sample_rate=clip['sample_rate'],
                           keys=[dict(time=key['m_Time'], sprite=index(sprite_key(name, key)),
                                      source=sprite_key(name, key)) for key in clip['keys']])
    roots = {int(row['go'].rsplit('-', 1)[1][:-5]): row for row in mapping['inventory']}
    plants, passive = [], []
    for plant in profile['plants'] + profile.get('passive_plants', []):
        owner = plant['owner']
        authored = index(roots[owner]['sprite'])
        selected = [draw for draw in draws if draw['owner'] == owner]
        require(len(selected) == 1 and selected[0]['sprite'] == authored,
                'Plant authored draw missing or split')
        target = passive if 'random_start' in plant else plants
        record = dict(owner=owner, authored_sprite=authored, clip=plant['clip'], state=plant['state'])
        if target is passive:
            record['random_start'] = plant['random_start']
        target.append(record)
    result = dict(schema=1, build=profile['build'], scene=profile['scene'],
                  room_manifest_sha256=digest(room_manifest),
                  dependency_manifest_sha256=profile['source_manifest_sha256'],
                  behavior_sha256=profile['behavior_sha256'],
                  room_visual_sha256=digest(binary), plants=plants, passive_plants=passive, clips=clips,
                  limitations=[profile['provenance_limitation'],
                               'Artwork binding only; no native Animator or RNG policy.'])
    validate(result, binary)
    return result


def validate(binding, binary):
    require(binding['schema'] == 1 and binary[:4] in (b'HKR2', b'HKR3', b'HKR4') and len(binary) >= 20,
            'Invalid plant artwork format')
    require(binding['room_visual_sha256'] == digest(binary), 'Changed plant room artwork')
    _, _, sprites, draw_count, grass_count = struct.unpack_from('<4s4I', binary)
    require((binary[:4] == b'HKR2' and grass_count == 0) or binary[:4] in (b'HKR3', b'HKR4'),
            'Invalid plant room header')
    draw_start = 20 + sprites * 36
    grass_start = draw_start + draw_count * 72
    expected = grass_start + (grass_count * 24 if binary[:4] in (b'HKR3', b'HKR4') else 0)
    expected += 4 if binary[:4] == b'HKR4' else 0
    require(len(binary) == expected,
            'Invalid plant room layout')
    draws = [struct.unpack_from('<2I', binary, draw_start + i * 72)
             for i in range(draw_count)]
    owners = set()
    for plant in binding['plants'] + binding.get('passive_plants', []):
        require(plant['owner'] not in owners, 'Duplicate plant owner')
        owners.add(plant['owner'])
        require(0 <= plant['authored_sprite'] < sprites and
                [sprite for sprite, owner in draws if owner == plant['owner']] ==
                [plant['authored_sprite']], 'Invalid authored plant draw')
        require(plant['clip'] in binding['clips'], 'Missing plant clip binding')
    for clip in binding['clips'].values():
        duration = clip['duration']
        require(math.isfinite(duration) and duration > 0 and clip['keys'], 'Invalid plant duration')
        previous = -1
        for key in clip['keys']:
            require(math.isfinite(key['time']) and previous < key['time'] <= duration and
                    0 <= key['sprite'] < sprites, 'Invalid plant key binding')
            previous = key['time']
        require(clip['keys'][0]['time'] == 0, 'Missing plant initial key')
    if binary[:4] in (b'HKR3', b'HKR4'):
        previous = -1
        for i in range(grass_count):
            draw, *values = struct.unpack_from('<I5f', binary, grass_start + i * 24)
            require(previous < draw < draw_count and all(math.isfinite(v) for v in values),
                    'Invalid grass sway table')
            previous = draw


def resident_binding(binding, original, resident):
    """Atlas repacking may change rectangles, never sprite geometry or draw identity."""
    validate(binding, original)
    require(len(original) == len(resident) and original[8:20] == resident[8:20],
            'Repacked plant room identity changed')
    sprites = struct.unpack_from('<I', original, 8)[0]
    require(original[20 + sprites * 36:] == resident[20 + sprites * 36:] and
            all(original[20+i*36+20:20+(i+1)*36] == resident[20+i*36+20:20+(i+1)*36]
                for i in range(sprites)), 'Repacked plant geometry/draws changed')
    result = dict(binding, room_visual_sha256=digest(resident),
                  source_room_visual_sha256=digest(original))
    validate(result, resident)
    return result
