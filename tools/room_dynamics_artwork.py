"""Source-backed sprite admission and bounded room animation/motion bank."""
import converted_files  # Install hardlink copy-on-write before asset writes.
import hashlib
import json
import math
import struct

from assetripper_client import ROOT


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def sources(source, room, mapping):
    manifest = json.loads((source/'manifest.json').read_bytes())
    require((manifest['build'], manifest['scene']) == (22529139, 'Tutorial_01'), 'Wrong dynamics source')
    require(digest((room/'room-visuals-map.json').read_bytes()) == manifest['room_manifest_sha256'],
            'Changed dynamics room anchors')
    for name, sha in manifest['source_sha256'].items():
        require(digest((source/name).read_bytes()) == sha, 'Changed dynamics dependency: '+name)
    for name, sha in manifest['room_anchors'].items():
        require(digest((room/name).read_bytes()) == sha, 'Changed spike Animator: '+name)
    spikes = manifest['spikes']
    require(len(spikes) == 32 and len({s['owner'] for s in spikes}) == 32, 'Expected 32 unique spikes')
    controllers = {s['controller'] for s in spikes}
    clips = {c for s in spikes for c in s['clips']}
    require(len(controllers) == len(clips) == 1, 'Expected shared spike controller/clip')
    controller = json.loads((source/next(iter(controllers))).read_bytes())
    machine = controller['m_Controller']['m_StateMachineArray'][0]['m_Data']
    require(machine['m_DefaultState'] == 0 and len(machine['m_StateConstantArray']) == 1,
            'Unsupported spike state machine')
    state = machine['m_StateConstantArray'][0]['m_Data']
    require(state['m_Speed'] == 1 and state['m_CycleOffset'] == 0 and state['m_Loop'] and
            not state['m_TransitionConstantArray'], 'Changed spike clock policy')
    for spike in spikes:
        animator = json.loads((room/spike['animator']).read_bytes())
        require(animator['m_CullingMode'] == 2 and animator['m_UpdateMode'] == 0 and animator['m_Enabled'],
                'Unsupported spike Animator update policy')
    clip = json.loads((source/next(iter(clips))).read_bytes())
    require(clip['m_Name'] == 'Spikes_anim' and len(clip['m_PPtrCurves']) == 1 and
            not any(clip[k] for k in ('m_FloatCurves', 'm_PositionCurves', 'm_RotationCurves', 'm_ScaleCurves')),
            'Expected sprite-only Spikes_anim')
    curve = clip['m_PPtrCurves'][0]
    require(curve['m_Path'] == '' and curve['m_Attribute'] == 'm_Sprite', 'Wrong spike sprite curve')
    duration = clip['m_MuscleClip']['m_StopTime']
    require(clip['m_MuscleClip']['m_LoopTime'] and len(curve['m_Curve']) == 4, 'Changed spike loop')
    # These PPtrs resolve to resources.assets through sharedassets6's dependency table.
    keys = [dict(time=k['m_Time'], source=f"resources.assets-{k['m_Value']['m_PathID']}.json")
            for k in curve['m_Curve']]
    extra = {}
    for key in keys:
        name = key['source']
        sprite = json.loads((source/name).read_bytes())
        texture = f"resources.assets-{sprite['m_RD']['m_Texture']['m_PathID']}.json"
        extra[name] = (source/manifest['textures'][texture], sprite)
        if name in mapping['sprites']:
            require(digest((room/name).read_bytes()) == manifest['source_sha256'][name],
                    'Conflicting spike sprite')
    rocks = [r for r in mapping['inventory'] if r['active'] and 'SimpleRock' in r['components']]
    require(len(rocks) == 8, 'Expected eight SimpleRocks')
    whole = {k['source'] for k in keys} | {r['sprite'] for r in rocks}
    return dict(duration=duration, keys=keys, spikes=spikes, rocks=rocks,
                children={owner: [json.loads((source/name).read_bytes()) for name in names]
                          for owner, names in manifest['rock_children'].items()},
                materials={owner: json.loads((source/name).read_bytes())
                           for owner, name in manifest['rock_materials'].items()}), extra, whole


def crumble_binding(source, mapping, draws):
    from breakable_owner_effects import crumble_floors
    result = []
    for floor in crumble_floors(source, mapping):
        angle = math.radians(floor['degrees']); c,s = math.cos(angle),math.sin(angle)
        px,py = floor['pivot']
        for i,d in enumerate(draws):
            if d['owner'] not in floor['owners']:
                continue
            v = d['values']; x,y = v[2]-px,v[5]-py
            result.append(dict(draw=i, owner=d['owner'], values=[
                c*v[0]-s*v[3], c*v[1]-s*v[4], px+c*x-s*y,
                s*v[0]+c*v[3], s*v[1]+c*v[4], py+s*x+c*y]))
    # Break destroys every child of Self, including the two unrotated solids.
    children = {int(r['go'][7:-5]) for r in mapping['inventory']
                if r['active'] and 'level6-318.json' in r['owners']}
    posed = {d['draw'] for d in result}
    for i,d in enumerate(draws):
        if d['owner'] in children and i not in posed:
            result.append(dict(draw=i,owner=d['owner'],values=list(d['values'][:6])))
    result.sort(key=lambda d:(d['owner'],d['draw']))
    require(0 < len(result) <= 32, 'Crumble draw budget exceeded')
    # The sidecar and room bank describe the same float32 runtime poses.
    for draw in result:
        draw['values']=list(struct.unpack('<6f',struct.pack('<6f',*draw['values'])))
    return result


def bind(profile, source, mapping, by_sprite, draws, binary):
    def read(name):
        raw = (source/name).read_bytes()
        require(digest(raw) == mapping['source_sha256'][name], 'Changed rock component: '+name)
        return json.loads(raw)

    def index(name):
        require(len(by_sprite.get(name, [])) == 1, 'Dynamics sprite must be whole: '+name)
        return by_sprite[name][0]

    def draw(owner):
        rows = [d for d in draws if d['owner'] == owner]
        require(len(rows) == 1, 'Missing or split dynamics owner')
        return rows[0]

    spikes = []
    for spike in profile['spikes']:
        d = draw(spike['owner'])
        spikes.append(dict(owner=spike['owner'], authored_sprite=d['sprite']))
    settings = json.loads((ROOT/'_private/research/assets/Physics2DSettings.json').read_bytes())
    rocks = []
    for row in profile['rocks']:
        owner = int(row['go'][7:-5]); d = draw(owner)
        body = read(row['components']['Rigidbody2D'][0])
        boxes = [read(k) for k in row['components']['BoxCollider2D'] if 'm_IsTrigger' in read(k)]
        solid = [b for b in boxes if not b['m_IsTrigger']]
        child = profile['children'][row['go']]
        trigger = [c for c in child if c.get('m_IsTrigger')]
        child_transform = next(c for c in child if 'm_LocalPosition' in c)
        require(len(solid) == len(trigger) == 1 and body['m_Mass'] > 0 and
                body['m_LinearDamping'] == 0 and body['m_Constraints'] == 0, 'Unsupported rock physics')
        bounce = read(row['components']['ObjectBounce'][0])['m_Structure']
        require(not any(bounce[k] for k in ('playSound', 'playAnimationOnBounce', 'sendFSMEvent')),
                'Unsupported rock bounce event')
        transforms = [read(k) for k in row['chain']]
        sx = math.prod(t['m_LocalScale']['m_X'] for t in transforms)
        sy = math.prod(t['m_LocalScale']['m_Y'] for t in transforms)
        def bounds(box):
            x, y = box['m_Offset']['m_X']*sx, box['m_Offset']['m_Y']*sy
            w, h = box['m_Size']['m_X']*abs(sx), box['m_Size']['m_Y']*abs(sy)
            return [x-w/2, y-h/2, x+w/2, y+h/2]
        collider = bounds(solid[0])
        child_scale, child_position = child_transform['m_LocalScale'], child_transform['m_LocalPosition']
        radius = trigger[0]['m_Radius']*max(abs(sx*child_scale['m_X']),abs(sy*child_scale['m_Y']))
        x,y = child_position['m_X']*sx,child_position['m_Y']*sy
        sensor = [x-radius,y-radius,x+radius,y+radius]
        inertia = body['m_Mass']*((collider[2]-collider[0])**2+(collider[3]-collider[1])**2)/12
        material = profile['materials'][row['go']]
        rocks.append(dict(owner=owner, spawn=[d['values'][2], d['values'][5]], collider=collider,
                          trigger=sensor, mass=body['m_Mass'], inertia=inertia,
                          material=[material['m_Friction'], material['m_Bounciness']],
                          gravity=settings['m_Gravity']['m_Y']*body['m_GravityScale'],
                          damping=body['m_AngularDamping'], bounce=bounce['bounceFactor'],
                          threshold=bounce['speedThreshold']))
        index(row['sprite'])
    crumble = crumble_binding(source, mapping, draws)
    return dict(schema=1, room_visual_sha256=digest(binary), duration=profile['duration'], crumble=crumble,
                keys=[dict(time=k['time'], sprite=index(k['source'])) for k in profile['keys']],
                spikes=spikes, rocks=rocks)


def compile_bank(binding, binary):
    require(binding['room_visual_sha256'] == digest(binary), 'Changed dynamics artwork')
    _, _, sprites, draws, _ = struct.unpack_from('<4s4I', binary)
    rows = [struct.unpack_from('<2I', binary, 20+sprites*36+i*72) for i in range(draws)]
    require(len(binding['keys']) == 4 and len(binding['spikes']) == 32 and len(binding['rocks']) == 8,
            'Invalid dynamics counts')
    require(math.isfinite(binding['duration']) and binding['duration'] > 0, 'Invalid spike duration')
    result = bytearray(struct.pack('<4s2If', b'HKD1', sprites, draws, binding['duration']))
    previous = -1
    for key in binding['keys']:
        require(previous < key['time'] < binding['duration'] and 0 <= key['sprite'] < sprites,
                'Invalid spike key')
        previous = key['time']; result += struct.pack('<fI', key['time'], key['sprite'])
    require(binding['keys'][0]['time'] == 0, 'Missing initial spike key')
    owners = set()
    for spike in binding['spikes']:
        require(spike['owner'] not in owners and [s for s, o in rows if o == spike['owner']] ==
                [spike['authored_sprite']], 'Invalid spike binding')
        owners.add(spike['owner']); result += struct.pack('<2I', spike['owner'], spike['authored_sprite'])
    for rock in binding['rocks']:
        require(rock['owner'] not in owners and sum(o == rock['owner'] for _, o in rows) == 1,
                'Invalid rock binding')
        owners.add(rock['owner'])
        values = rock['spawn']+rock['collider']+rock['trigger']+rock['material']+[rock[k] for k in
                  ('mass', 'inertia', 'gravity', 'damping', 'bounce', 'threshold')]
        require(all(math.isfinite(v) for v in values), 'Nonfinite rock config')
        require(rock['mass'] > 0 and rock['inertia'] > 0 and rock['gravity'] <= 0 and
                rock['damping'] >= 0 and 0 <= rock['bounce'] <= 1 and rock['threshold'] >= 0,
                'Invalid rock dynamics')
        result += struct.pack('<I18f', rock['owner'], *values)
    if 'crumble' in binding:
        crumble = binding['crumble']
        require(0 < len(crumble) <= 32, 'Invalid Crumble count')
        result += struct.pack('<4sI', b'CRB1', len(crumble))
        seen = set()
        for d in crumble:
            i,owner,values = d['draw'],d['owner'],d['values']
            require(i not in seen and 0 <= i < draws and rows[i][1] == owner and
                    owner not in owners, 'Invalid Crumble binding')
            require(len(values) == 6 and all(math.isfinite(v) for v in values), 'Invalid Crumble pose')
            seen.add(i); result += struct.pack('<2I6f', i, owner, *values)
        require([d['owner'] for d in crumble] == sorted(d['owner'] for d in crumble),
                'Crumble owners must be sorted for visibility lookup')
    return bytes(result)


def refresh_crumble(output):
    """Regenerate motion metadata against unchanged, already converted artwork."""
    source = ROOT/'_private/research/room-visuals'
    mapping = json.loads((source/'room-visuals-map.json').read_bytes())
    binary = (output/'room-visual.bin').read_bytes()
    _,_,sprites,count,_ = struct.unpack_from('<4s4I', binary)
    draws = []
    for i in range(count):
        row = struct.unpack_from('<2I16f', binary, 20+sprites*36+i*72)
        draws.append(dict(owner=row[1], values=row[2:]))
    path = output/'room-dynamics.json'
    binding = json.loads(path.read_bytes())
    compile_bank({k:v for k,v in binding.items() if k!='crumble'},binary)
    # The previous extension may use an older layout; validate its replacement below.
    binding['crumble'] = crumble_binding(source,mapping,draws)
    (output/'room-dynamics.bin').write_bytes(compile_bank(binding,binary))
    path.write_text(json.dumps(binding,indent=2)+'\n')
    manifest_path = output/'manifest.json'
    manifest = json.loads(manifest_path.read_bytes())
    manifest['room_dynamics_sha256'] = digest(path.read_bytes())
    manifest_path.write_text(json.dumps(manifest,indent=2)+'\n')
    print(f"Crumble: {len(binding['crumble'])} draws, unchanged room artwork")


if __name__ == '__main__':
    import argparse
    from pathlib import Path
    parser = argparse.ArgumentParser(description='Refresh source-derived Crumble metadata without repacking artwork')
    parser.add_argument('--refresh-crumble',type=Path,required=True)
    refresh_crumble(parser.parse_args().refresh_crumble)
