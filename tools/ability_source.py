"""Read-only build22529139 pickup evidence; all exports stay in this checkout.

Run with Python3.12 and --extract once, then regenerate from the saved snapshot.
UnityPy and original data are supplied by HKPSP_SOURCE_ROOT, never installed here.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
SOURCE = Path(os.environ.get('HKPSP_SOURCE_ROOT', next((p / 'HK3DS' for p in ROOT.parents if (p / 'HK3DS').is_dir()), ROOT.parent / 'HK3DS')))
SNAPSHOT = ROOT / '_private/converted-psp/ability-source'
PICKUPS = {'cloak': ('level132', 5138), 'claw': ('level178', 7543),
           'heart': ('level270', 6570), 'key': ('level97', 5820)}


def extract(output):
    output = output.resolve()
    if not output.is_relative_to((ROOT / '_private/converted-psp').resolve()):
        raise ValueError('Pickup exports must stay in this checkout private PSP tree')
    sys.path.insert(0, str(SOURCE / '_private/tools/boss-python'))
    import UnityPy
    from watcher_unity import AlignedGenerator
    from inspect_fsm import decode_fsm
    source = SOURCE / '_private/original/Hollow Knight/hollow_knight_Data'
    env = UnityPy.Environment(path=str(source))
    generator = AlignedGenerator('6000.0.61f1')
    generator.load_local_dll_folder(str(source / 'Managed'))
    env.typetree_generator = generator
    files, hashes = {}, {}
    output.mkdir(parents=True, exist_ok=True)

    def load(name):
        if name not in files:
            path = source / name
            hashes[name] = hashlib.sha256(path.read_bytes()).hexdigest()
            env.load_file(str(path))
            files[name] = next(f for f in env.files.values() if getattr(f, 'name', '') == name)
        return files[name]

    def resolve(owner, ref):
        name = owner.name if ref['m_FileID'] == 0 else Path(owner.externals[ref['m_FileID']-1].path).name
        return load(name).objects[ref['m_PathID']]

    def parts(go):
        return [resolve(go.assets_file, c['component']) for c in go.read_typetree()['m_Component']]

    def component_record(component, value):
        record = {'type': component.type.name,
                  'ref': [component.assets_file.name, component.path_id], 'data': value}
        if 'm_fontAsset' in value:
            font = resolve(component.assets_file, value['m_fontAsset'])
            record['font_ref'] = [font.assets_file.name, font.path_id]
            record['font_name'] = font.read_typetree()['m_Name']
        return record

    def fsm(name, ident):
        raw = load(name).objects[ident].read_typetree()
        return decode_fsm(raw['fsm'])

    def descendants(go):
        result = [go.path_id]
        tr = next(c for c in parts(go) if c.type.name in ('Transform', 'RectTransform'))
        for child in tr.read_typetree()['m_Children']:
            child_tr = resolve(tr.assets_file, child)
            result.extend(descendants(resolve(child_tr.assets_file, child_tr.read_typetree()['m_GameObject'])))
        return result

    data = {'build': 22529139, 'pickups': {}, 'sprites': {}, 'panel': [], 'hero_clips': {}, 'panel_animations': {}}
    for name, (scene, ident) in PICKUPS.items():
        original = load(scene).objects[ident].read_typetree()
        d = fsm(scene, ident)
        go = resolve(load(scene), original['m_GameObject'])
        transform = next(c.read_typetree() for c in parts(go) if c.type.name == 'Transform')
        data['pickups'][name] = {'ref': [scene, ident], 'fsm': d, 'transform': transform,
                                'owners': descendants(go)}
    data['superdash'] = fsm('resources.assets', 23052)
    data['heart_events'] = []
    for obj in load('level270').objects.values():
        if obj.type.name != 'MonoBehaviour':
            continue
        raw = obj.get_raw_data()
        if b'SUPERDASH BLAST' not in raw and b'MANTIS FLOOR FINISH' not in raw:
            continue
        component = obj.read_typetree()
        if 'fsm' in component:
            go = resolve(obj.assets_file, component['m_GameObject'])
            data['heart_events'].append({'name': go.read_typetree()['m_Name'],
                'ref': ['level270', obj.path_id], 'owners': descendants(go),
                'fsm': decode_fsm(component['fsm']),
                'components': [{'ref': [c.assets_file.name, c.path_id], 'type': c.type.name,
                                'data': c.read_typetree()} for c in parts(go)]})
    data['panel_fsm'] = fsm('sharedassets6.assets', 1180)
    res = load('resources.assets')
    data['hud_cameras'] = []
    for component in res.objects.values():
        if component.type.name != 'Camera':
            continue
        camera = component.read_typetree()
        owner = resolve(res, camera['m_GameObject'])
        if owner.read_typetree()['m_Name'] == 'HudCamera':
            data['hud_cameras'].append({'ref': [res.name, component.path_id], 'data': camera})
    for ident in (4466, 7664):
        go = res.objects[ident]
        tr = next(c.read_typetree() for c in parts(go) if c.type.name == 'Transform')
        data[go.read_typetree()['m_Name']] = tr
    for component in parts(res.objects[3916]):
        if component.type.name != 'MonoBehaviour':
            continue
        d = component.read_typetree()
        if 'library' in d:
            library = resolve(res, d['library'])
            clips = library.read_typetree()['clips']
            data['hero_library'] = [library.assets_file.name, library.path_id]
            data['hero_clips'] = {c['name']: c for c in clips if
                c['name'].startswith(('Collect Normal', 'Collect SD')) or c['name'] == 'GetUpToIdle'}

    def sprite(alias, obj, scale=(1., 1.)):
        s = obj.read()
        image = s.image.convert('RGBA')
        image.save(output / (alias + '.png'))
        rect, pivot, offset, ppu = s.m_Rect, s.m_Pivot, s.m_RD.textureRectOffset, s.m_PixelsToUnits
        left, bottom = (offset.x-pivot.x*rect.width)/ppu, (offset.y-pivot.y*rect.height)/ppu
        data['sprites'][alias] = {'ref': [obj.assets_file.name, obj.path_id],
            'name': s.m_Name,
            'bounds': [left*scale[0], bottom*scale[1],
                       (left+image.width/ppu)*scale[0], (bottom+image.height/ppu)*scale[1]],
            'size': list(image.size)}

    # Keep the authored tk2d geometry/pivots, not a screenshot of the pose.
    from convert_hero import decode_sprite, trim_transparent
    data['hero_sprites'] = []
    frame_indices, collections = {}, {}
    for clip in data['hero_clips'].values():
        clip['export_frames'] = []
        for frame in clip['frames']:
            collection_obj = resolve(res, frame['spriteCollection'])
            key = (collection_obj.assets_file.name, collection_obj.path_id, frame['spriteId'])
            if key not in frame_indices:
                if key[:2] not in collections:
                    collection = collection_obj.read_typetree()
                    atlas = resolve(collection_obj.assets_file, collection['textures'][0]).read().image.convert('RGBA')
                    collections[key[:2]] = collection, atlas
                collection, atlas = collections[key[:2]]
                image, bounds = decode_sprite(atlas, collection['spriteDefinitions'][frame['spriteId']])
                image, bounds = trim_transparent(image, bounds)
                index = len(data['hero_sprites'])
                filename = f'hero-{index}.png'
                image.save(output / filename)
                data['hero_sprites'].append({'ref': list(key), 'file': filename,
                                            'bounds': bounds, 'size': list(image.size)})
                frame_indices[key] = index
            clip['export_frames'].append(frame_indices[key])

    # UIButtonSkins22457 is the source used by ActionButtonIconBase.GetButtonIcon.
    # Cross is the PSP bottom face; original Switch bumper art spells L/R without
    # the PS4 L1/R1 suffix, matching controls.md's single-shoulder mapping.
    skins = res.objects[22457].read_typetree()
    data['button_skins'] = {'ref': [res.name, 22457], 'mapping': {}}
    for alias, field in (('button-cross', 'ps4x'), ('button-l', 'switchHidLeftBumper'),
                         ('button-r', 'switchHidRightBumper')):
        obj = resolve(res, skins[field])
        sprite(alias, obj)
        data['button_skins']['mapping'][alias] = {
            'field': field, 'ref': [obj.assets_file.name, obj.path_id]}

    # Icon references from Dash/Walljump serialized Icon Sprite and Heart Prompt.
    sprite('key', res.objects[3700])
    for name, state in (('cloak', 'Dash'), ('claw', 'Walljump'), ('heart', 'Prompt')):
        d = data['pickups'][name]['fsm']
        if name == 'heart':
            a = next(a for s in d['states'] if s['name'] == state for a in s['actions']
                     if a['type'].endswith('.CreateUIMsgGetItem') and a['enabled'])
            ref = a['parameters']['sprite']['value']
        else:
            ref = next(x['value'] for x in d['variables']['objectVariables'] if x['name'] == 'Icon Sprite')
        sprite(name, resolve(load(PICKUPS[name][0]), ref), (1.5607432127, 1.5607424974))

    from convert_monomon_mask import streamed_channels
    burst = res.objects[1130].read_typetree()
    burst_channels = streamed_channels(burst)
    root_scale = res.objects[9607].read_typetree()['m_LocalScale']
    child_scale = res.objects[9588].read_typetree()['m_LocalScale']
    child_position = res.objects[9588].read_typetree()['m_LocalPosition']
    burst_frames = []
    for i, key in enumerate(row for row in burst_channels[1] if math.isfinite(row[0])):
        alias = 'pickup-burst-'+str(i)
        sprite(alias, resolve(res, burst['m_ClipBindingConstant']['pptrCurveMapping'][round(key[-1])]),
               (root_scale['x']*child_scale['x'], root_scale['y']*child_scale['y']))
        burst_frames.append({'time': key[0], 'sprite': alias})
    data['pickup_burst'] = {'ref': ['resources.assets', 1130], 'fps': burst['m_SampleRate'],
                            'duration': burst['m_MuscleClip']['m_StopTime'], 'frames': burst_frames,
                            'depth': .004-1+root_scale['z']*child_position['z'],
                            'alpha': burst_channels[0]}

    # Every child carries its original transform, fade FSM and renderer data.
    panel = load('sharedassets6.assets')
    parent = panel.objects[605].read_typetree()
    for child in parent['m_Children']:
        tr = resolve(panel, child).read_typetree()
        go = resolve(panel, tr['m_GameObject'])
        name = go.read_typetree()['m_Name']
        record = {'name': name, 'transform': tr, 'components': []}
        for c in parts(go):
            d = c.read_typetree()
            record['components'].append(component_record(c, d))
            if c.type.name == 'Animator':
                controller = resolve(c.assets_file, d['m_Controller'])
                native = controller.read_typetree()
                for clip_ref in native['m_AnimationClips']:
                    clip_obj = resolve(controller.assets_file, clip_ref)
                    clip = clip_obj.read_typetree()
                    from convert_monomon_mask import streamed_channels
                    bindings = clip['m_ClipBindingConstant']
                    channels = streamed_channels(clip)
                    if [(b['typeID'], b['attribute'], b['isPPtrCurve']) for b in bindings['genericBindings']] != [
                            (212, 304273561, 0), (212, 0, 1)]:
                        raise ValueError('Unsupported original panel animation bindings')
                    # CRC32(m_Color.a), then the native SpriteRenderer PPtr channel.
                    alpha = [row for row in channels[0] if math.isfinite(row[0])]
                    keys = [row for row in channels[1] if math.isfinite(row[0])]
                    frames = []
                    for i, key in enumerate(keys):
                        alias = name.lower()+f'-{i}'
                        scale = tr['m_LocalScale']
                        ref = bindings['pptrCurveMapping'][round(key[-1])]
                        sprite(alias, resolve(clip_obj.assets_file, ref), (scale['x'], scale['y']))
                        frames.append({'time': key[0], 'sprite': alias})
                    data['panel_animations'][name.lower()] = {'ref': [clip_obj.assets_file.name, clip_obj.path_id],
                        'name': clip['m_Name'], 'fps': clip['m_SampleRate'],
                        'loop': clip['m_MuscleClip']['m_LoopTime'],
                        'frames': frames, 'alpha': alpha,
                        'duration': clip['m_MuscleClip']['m_StopTime']}
            if c.type.name == 'SpriteRenderer' and name in ('Fleur', 'Stop', 'BG'):
                alias = name.lower()
                scale = tr['m_LocalScale']
                sprite(alias, resolve(panel, d['m_Sprite']), (scale['x'], scale['y']))
        data['panel'].append(record)

    # Shiny Control Trink Flash spawns this pooled prefab (level97 external4).
    key_fsm = data['pickups']['key']['fsm']
    spawn = next(a for s in key_fsm['states'] if s['name'] == 'Trink Flash'
                 for a in s['actions'] if a['enabled'] and a['type'].endswith('.SpawnObjectFromGlobalPool')
                 and a['parameters']['storeObject']['name'] == 'Msg')
    popup = resolve(load('level97'), spawn['parameters']['gameObject']['value'])
    data['trinket'] = []
    def tree(go):
        record = {'name': go.read_typetree()['m_Name'], 'ref': [go.assets_file.name, go.path_id], 'components': []}
        transform = None
        for c in parts(go):
            d = c.read_typetree()
            record['components'].append(component_record(c, d))
            if c.type.name in ('Transform', 'RectTransform'):
                transform = d
            if c.type.name == 'SpriteRenderer' and record['name'] == 'pop_up_backboard':
                tr = next(p.read_typetree() for p in parts(go) if p.type.name == 'Transform')
                sprite('trink_bg', resolve(c.assets_file, d['m_Sprite']),
                       (tr['m_LocalScale']['x'], tr['m_LocalScale']['y']))
        data['trinket'].append(record)
        if transform:
            for child in transform['m_Children']:
                tr_obj = resolve(go.assets_file, child)
                tree(resolve(tr_obj.assets_file, tr_obj.read_typetree()['m_GameObject']))
    tree(popup)
    from ability_effects import extract as extract_effects
    data['effects'] = extract_effects(load, resolve, parts, data['pickups'], data['heart_events'], output)
    data['localization'] = {}
    for sheet, path in [('UI', 'cornifer/EN_UI-decrypted.xml'),
                        ('Prompts', 'tutorial-reminders/EN_Prompts-decrypted.xml')]:
        path = SOURCE / '_private/research' / path
        hashes[str(path.relative_to(SOURCE))] = hashlib.sha256(path.read_bytes()).hexdigest()
        entries = {e.attrib['name']: e.text for e in ET.parse(path).iter('entry')}
        keys = {'INV_NAME_DASH', 'INV_NAME_WALLJUMP', 'INV_NAME_SUPERDASH', 'INV_NAME_SIMPLEKEY',
                'GET_ITEM_INTRO1', 'GET_ITEM_INTRO2', 'BUTTON_DESC_PRESS', 'BUTTON_DESC_HOLD',
                'GET_DASH_1', 'GET_DASH_2', 'GET_WALLJUMP_1', 'GET_WALLJUMP_2',
                'GET_SUPERDASH_1', 'GET_SUPERDASH_2'}
        data['localization'][sheet] = {k: v for k, v in entries.items() if k in keys}
    data['source_sha256'] = hashes
    (output / 'snapshot.json').write_text(json.dumps(data, indent=1, ensure_ascii=True), encoding='utf-8')
    return data


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--extract', action='store_true')
    parser.add_argument('--output', type=Path, default=SNAPSHOT)
    args = parser.parse_args()
    if args.extract:
        d = extract(args.output)
    else:
        d = json.loads((args.output / 'snapshot.json').read_text(encoding='utf-8'))
    print(f"Ability sources: {len(d['pickups'])} pickups, {len(d['sprites'])} sprites, build {d['build']}")
