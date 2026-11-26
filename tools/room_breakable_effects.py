"""Room-local breakable particle artwork and bindings; no original-player queries."""
import math
import copy
import struct
from types import SimpleNamespace
from room_breakable_fling import fling_emitter
from room_breakable_spells import spell_emitter

from PIL import Image


def shape_pose(emitter, shape, transforms, angle):
    """Project source Shape position/rotation/scale for each post-hit root angle."""
    from convert_room_visuals import project
    x, y, z = (math.radians(shape['m_Rotation']['m_' + a]) * .5 for a in 'XYZ')
    cx, sx, cy, sy, cz, sz = math.cos(x), math.sin(x), math.cos(y), math.sin(y), math.cos(z), math.sin(z)
    pose = dict(m_LocalPosition=shape['m_Position'], m_LocalScale=shape['m_Scale'],
                m_LocalRotation=dict(m_X=cy*sx*cz+sy*cx*sz, m_Y=sy*cx*cz-cy*sx*sz,
                                     m_Z=cy*cx*sz-sy*sx*cz, m_W=cy*cx*cz+sy*sx*sz))
    bases, axes, origins = [], [], []
    q = transforms[0]['m_LocalRotation']
    local = 2 * math.atan2(q['m_Z'], q['m_W'])
    kind = shape['m_Type']
    for delta in (0, -angle, angle):
        matrix = copy.deepcopy(transforms)
        rotation = local + math.radians(delta)
        if delta:
            matrix[0]['m_LocalRotation'] = dict(m_X=0, m_Y=0, m_Z=math.sin(rotation/2), m_W=math.cos(rotation/2))
        root = project((0, 0, 0), matrix)
        matrix = [pose, *matrix]
        center = project((0, 0, 0), matrix)
        origins.extend(v - o for v, o in zip(center, root))
        if kind == 10:
            radius = shape['m_RadiusParameter']['m_Value']
            for axis in ((1, 0, 0), (0, 1, 0)):
                column = [v-o for v, o in zip(project(axis, matrix), center)]
                bases.extend(v * radius for v in column[:2])
                axes.extend(column)
        else:
            direction = [v-o for v, o in zip(project((0, 0, 1), matrix), center)]
            length = math.sqrt(sum(v*v for v in direction))
            bases.extend(v / length if length else 0 for v in direction)
            for axis in range(3):
                point = [0, 0, 0]
                point[axis] = 1 if kind == 5 else 2 * shape['m_RadiusParameter']['m_Value'] if axis == 0 else 0
                axes.extend(v-o for v, o in zip(project(point, matrix)[:2], center[:2]))
    emitter.update(bases=bases + [0] * (12-len(bases)), axes=axes, shape_origins=origins)


def particle_choices(data):
    """Preserve null/external outcomes as well as locally admitted particles."""
    choices = [dict(weight=float(row['probability']), emitters=[])
               for row in data.get('containingParticles', [])]
    total = sum(choice['weight'] for choice in choices)
    if any(not math.isfinite(c['weight']) or c['weight'] < 0 for c in choices) or not math.isfinite(total) or (choices and total <= 0):
        raise ValueError('Invalid room breakable particle probabilities')
    return choices


def pack_profiles(profiles, spells=False):
    if len(profiles) > 127 or sum(len(p['choices']) for p in profiles) > 1024:
        raise ValueError('Room breakable particle choice metadata exceeds capacity')
    references = sum(len(p['emitters']) + sum(len(c['emitters']) for c in p['choices']) +
                     len(p.get('spells', [])) for p in profiles)
    if references > 1024:
        raise ValueError('Room breakable particle references exceed capacity')
    binary = bytearray()
    for profile in profiles:
        binary += struct.pack('<3I', profile['owner'], len(profile['emitters']), len(profile['choices']))
        if spells:
            binary += struct.pack('<I', len(profile.get('spells', [])))
        binary += struct.pack('<' + 'I' * len(profile['emitters']), *profile['emitters'])
        for choice in profile['choices']:
            binary += struct.pack('<fI', choice['weight'], len(choice['emitters']))
            binary += struct.pack('<' + 'I' * len(choice['emitters']), *choice['emitters'])
        if spells:
            binary += struct.pack('<' + 'I' * len(profile.get('spells', [])), *profile.get('spells', []))
    return binary


def pack_force(e):
    curves = e.get('force_curves')
    if curves is not None and (len(curves) != 48 or
            not all(math.isfinite(v) and abs(v) <= 1e6 for v in curves)):
        raise ValueError('Invalid room particle force curves')
    return struct.pack('<I48f', int(curves is not None), *(curves or [0]*48))


def pack_emitter(e, first):
    material = e.get('material', 0)
    if material not in (0, 1, 4):
        raise ValueError('Unsupported room breakable particle material')
    values = [*e['values'], e['rate'], *e['cone'], *e['bases'], *e['axes'], *e['shape_origins'], *e['sorting']]
    if not all(math.isfinite(v) and abs(v) <= 1e6 for v in values):
        raise ValueError('Invalid room breakable particle values: ' + e['source'])
    # Unity accepts descending random endpoints. RBF admission uses positive
    # ordered intervals; sorting retains their uniform lifetime/size distribution.
    for lo in (7, 11):
        values[lo:lo+2] = sorted(values[lo:lo+2])
    return struct.pack('<4I113f', first + e['sprite'], e['tiles'], e['count'],
                       e['shape'] | (256 if e['visual'] else 0) | (material << 9), *values)


def compile_effects(mapping, source, read, tiles, ambient):
    # Import lazily: convert_decor_effects shares the room transform projector.
    from convert_decor_effects import emitter_factory

    objects = {row['go']: row for row in mapping.get('breakable_objects', [])}
    tables = mapping.get('collection_dependencies', {})

    def ref(key, value):
        collection = key.rsplit('-', 1)[0]
        if value.get('m_FileID'):
            collection = tables[collection][str(value['m_FileID'])]
        return collection + '-' + str(value['m_PathID']) + '.json'

    def chain(key):
        result, seen = [], set()
        while key:
            if key in seen:
                raise ValueError('Cyclic room particle transform: ' + key)
            seen.add(key)
            transform = read(key)
            result.append(transform)
            key = ref(key, transform['m_Father']) if transform['m_Father']['m_PathID'] else None
        return result

    media = mapping.get('breakable_prefabs', {}).get('effects', {}).get('media', {})
    images, geometry, identities, emitters = [], [], {}, []

    def image(identity, bitmap, bounds):
        if identity not in identities:
            identities[identity] = len(images)
            images.append(bitmap)
            geometry.append(bounds)
        return identities[identity]

    def texture(key):
        # SpriteRenderer prefab textures are exported by the hierarchy pass;
        # ParticleSystem textures are exported by the effect dependency pass.
        name = media[key]['png'] if key in media else mapping['breakable_prefabs']['textures'][key]
        return Image.open(source / name).convert('RGBA')

    empty = dict(objects={})
    owner_source = SimpleNamespace(objects=objects, instance_transform=lambda owner, transforms: transforms)
    emitter = emitter_factory(read, ref, chain, image, texture, images, identities, emitters,
                              owner_source, empty, dict(derived=empty),
                              dict(objects={}, groups=dict(land=[], flock=[])), empty, None)
    by_component, unconditional, choices, spells = {}, {}, {}, {}
    deferred, unsupported = [], {}
    for reference in mapping.get('breakable_references', []):
        key = reference['component']
        if key not in by_component:
            by_component[key], unconditional[key] = {}, []
            choices[key] = particle_choices(read(key)['m_Structure'])
            spells[key] = []
        if reference['field'] == 'spellHitEffectPrefab':
            effect = spell_emitter(reference, objects, read, ref, image, texture, ambient)
            if effect:
                spells[key].append(len(emitters))
                emitters.append(effect)
            else:
                deferred.append(reference)
            continue
        if reference['field'] not in ('debrisParts', 'flingDebris', 'containingParticles'):
            continue
        target = unconditional[key]
        if reference['field'] == 'containingParticles':
            index = int(reference['path'].split('/')[1])
            target = choices[key][index]['emitters']
        if not reference.get('scene_instance', True) or reference.get('mode', 'activate') != 'activate':
            fling = (fling_emitter(reference, objects, read, ref, image, texture, ambient)
                     if reference['field'] == 'containingParticles' else None)
            if fling:
                target.append(len(emitters))
                emitters.append(fling)
            else:
                deferred.append(reference)
            continue
        root = reference['root']
        component = read(key)
        data = component['m_Structure']
        owner = ref(key, component['m_GameObject'])
        go = read(owner)
        transform_key = next(ref(owner, c['m_Component']) for c in go['m_Components']
                             if 'm_LocalPosition' in read(ref(owner, c['m_Component'])))
        # Break rotates debrisParts; containingParticles only become active.
        angle = 0 if reference['field'] == 'containingParticles' else data.get('angleOffset', 0)
        if read(transform_key)['m_LocalScale']['m_X'] < 0:
            angle = -angle
        for row in objects.values():
            if root not in row['owners'] or 'ParticleSystem' not in row['components']:
                continue
            # SetActive(root) retains inactive children below that root.
            ancestors = row['owners'][:row['owners'].index(root)]
            if any(not read(parent).get('m_IsActive', True) for parent in ancestors):
                continue
            if row['go'] in unsupported:
                continue
            if row['go'] not in by_component[key]:
                saved = len(images), len(emitters), dict(identities)
                try:
                    emitter(row['go'], ambient=ambient, owned_angle=angle,
                            dust='dust' in read(row['go'])['m_Name'].lower())
                    shape_pose(emitters[-1], read(row['components']['ParticleSystem'][0])['m_ShapeModule'],
                               chain(row['components']['Transform'][0]), angle)
                except ValueError as error:
                    # An emitter feature the native pool cannot reproduce yet
                    # (loops, bursts, local forces, hierarchy scaling) stays
                    # unspawned and is reported, instead of failing the room.
                    if not str(error).startswith('Unsupported '):
                        raise
                    del images[saved[0]:], geometry[saved[0]:], emitters[saved[1]:]
                    identities.clear()
                    identities.update(saved[2])
                    unsupported[row['go']] = str(error)
                    deferred.append(dict(reference, unsupported=str(error)))
                    continue
                by_component[key][row['go']] = len(emitters) - 1
                emitters[-1]['component'] = key
            identifier = by_component[key][row['go']]
            if identifier not in target:
                target.append(identifier)
        if reference['field'] == 'containingParticles' and not target:
            deferred.append(reference)
    if len(emitters) > 256:
        raise ValueError('Room breakable particles exceed 256 emitters')
    bound = sum(e['count'] for e in emitters)
    if bound > 7296:
        raise ValueError('Room breakable particles exceed the shared particle pool')
    first = len(tiles)
    for bitmap, bounds in zip(images, geometry):
        tiles.append(dict(bitmap=bitmap, geometry=bounds, source='@breakable-particles', index=len(tiles)))
    profiles = []
    for key in sorted(by_component):
        component = read(key)
        owner = ref(key, component['m_GameObject'])
        profiles.append(dict(owner=int(owner.removesuffix('.json').rsplit('-', 1)[1]),
                             emitters=unconditional[key], choices=choices[key], spells=spells[key]))
    if len(profiles) > 127:
        raise ValueError('Room breakable particle profiles exceed 127 owners')
    binary = bytearray(struct.pack('<4s2I', b'RBF6', len(emitters), len(profiles)))
    for e in emitters:
        binary += pack_emitter(e, first)
        binary += struct.pack('<I5f', *e.get('fling', [0] * 6))
        binary += pack_force(e)
    binary += pack_profiles(profiles, spells=True)
    report = dict(emitters=emitters, profiles=profiles, particle_bound=bound,
                  sprites=len(images), pixels=sum(p.width * p.height for p in images),
                  deferred_spawns=deferred,
                  approximations=['Force-over-lifetime uses eight source curve samples per bound and axis; per-particle weights retained.',
                                  'Shared decor ParticleSystem projection and curve sampling; '
                                  'births use the source rate and duration in the shared emission scheduler.',
                                  'External SpatterOrange contact orientation uses the port terrain solver; '
                                  'seven-frame splash timing samples source FPS on the render clock.'])
    return bytes(binary), report
