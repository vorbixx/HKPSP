"""Pack RestingGrounds_09's source lever and station reveal; no toll or gate."""
import argparse
import copy
import hashlib
import json
import math
import struct
import subprocess
from pathlib import Path

import converted_files
from PIL import Image
from assetripper_client import ROOT
from atlas_packing import pack_pages
from convert_hero import decode_sprite
from convert_world_switch_visuals import bank_bytes
from inspect_fsm import decode_fsm
from private_output_paths import is_private_output
from scene_levers import signature, variable
from unity_transform import project
from convert_stag_feedback import compile_dust, write_audio
from tool_paths import TEX3DS as TEX3DS_EXE

SIGNATURES = {
    1693: '51541e3139f4469e21b52214f6370e44551a0b684d91ca91de60a99f1e3773b9',
    1842: '9be4cd2e3afe09ac408bc0e0cd2f89272d04f2d6d53fb63c7538b994d1d64458',
    1694: '064c2228be77fe934364d90d956ae3037024f93a550668dd292ed1777b5096e7',
}
OWNERS = (164, 446, 443, 444)


def require(ok, message):
    if not ok:
        raise ValueError(message)


def admit_graph(read):
    fsms = {i: decode_fsm(read(i)['m_Structure']['fsm']) for i in SIGNATURES}
    for ident, fsm in fsms.items():
        require(signature(fsm) == SIGNATURES[ident], f'Changed station FSM {ident}')
    lever, bell = fsms[1693], fsms[1842]
    for name in ('Activated', 'In Range', 'SetPlayerData'):
        require(variable(lever, 'boolVariables', name) is False, 'Changed lever startup')
    require(variable(lever, 'gameObjectVariables', 'Target 1') == dict(m_FileID=0, m_PathID=446), 'Changed station target')
    for name in ('Target 2', 'Target 3'):
        require(not variable(lever, 'gameObjectVariables', name)['m_PathID'], 'Unsupported extra lever target')
    require(not variable(bell, 'gameObjectVariables', 'Grate')['m_PathID'], 'Station gate needs a separate binding')
    require(variable(bell, 'stringVariables', 'PlayerData Bool') == 'openedRestingGrounds', 'Changed station flag')
    persistent = read(1907)['m_Structure']
    require(not persistent['semiPersistent'] and not persistent['dontSave'] and
            not persistent['persistentBoolData']['activated'], 'Changed lever persistence')

    def wait(fsm, name):
        actions = [a['parameters'] for s in fsm['states'] if s['name'] == name for a in s['actions']
                   if a['enabled'] and a['type'].split('.')[-1] == 'Wait']
        require(len(actions) == 1, 'Missing source Wait')
        action = actions[0]
        require(not action['realTime'] and not action['time']['useVariable'], 'Unsupported source clock')
        value = action['time']['value']
        require(math.isfinite(value) and 0 < value < 30, 'Invalid source Wait')
        return value
    return [wait(lever, 'Hit'), wait(bell, 'Pause Before Box Drop'), wait(bell, 'Bell Up Pause')]


def convert(art, source, output, tex3ds):
    art, source, output = art.resolve(), source.resolve(), output.absolute()
    require(is_private_output(output), 'Station output must stay private')
    manifest = json.loads((art / 'sources.json').read_text())
    require(manifest['scene'] == 'RestingGrounds_09', 'Wrong station scene')
    for name, digest in manifest['sha256'].items():
        path = (art / name).resolve()
        require(path.is_relative_to(art) and hashlib.sha256(path.read_bytes()).hexdigest() == digest, 'Changed source: ' + name)
    source_hashes = {}

    def read(ident):
        name = f'level244-{ident}.json'
        paths = (art / name, source / 'terrain' / name, source / 'visuals' / name)
        path = next((p for p in paths if p.is_file()), None)
        require(path is not None, 'Missing station source: ' + name)
        raw = path.read_bytes()
        source_hashes[str(path)] = hashlib.sha256(raw).hexdigest()
        return json.loads(raw)

    def part(owner, kind):
        entry = manifest['parts'][f'{owner}-{kind}']
        return entry['file'], json.loads((art / entry['file']).read_text())['m_Structure']

    times = admit_graph(read)
    owners = {row['id']: row for row in manifest['owners']}
    clips, sprites, remap, textures = [], [], {}, {}
    selected = {164: ('Lever Idle', 'Lever Hit', 'Lever Activated'),
                446: ('Box_idle', 'Bell_stand', 'Bell_stand_appear', 'Bell_idle', 'Front')}
    for owner, names in selected.items():
        collection_name, collection = part(owner, 'collection')
        _, library = part(owner, 'library')
        collection_id = int(Path(collection_name).stem.rsplit('-', 1)[1])
        require(len(collection['textures']) == 1, 'Unsupported station texture set')
        texture_ref = collection['textures'][0]
        require(texture_ref['m_FileID'] == 0, 'External station texture')
        texture_name = collection_name.rsplit('-', 1)[0] + '-' + str(texture_ref['m_PathID']) + '.png'
        require(texture_name in manifest['textures'], 'Missing source texture')
        textures[collection_name] = Image.open(art / texture_name).convert('RGBA')
        for name in names:
            found = [c for c in library['clips'] if c['name'] == name]
            require(len(found) == 1, 'Missing source clip: ' + name)
            clip = copy.deepcopy(found[0])
            require(clip['frames'] and math.isfinite(clip['fps']) and clip['fps'] > 0 and
                    clip['wrapMode'] in (0, 1, 2, 3, 6) and 0 <= clip['loopStart'] < len(clip['frames']), 'Invalid source clip')
            for frame in clip['frames']:
                require(frame['spriteCollection'] == dict(m_FileID=0, m_PathID=collection_id), 'External clip collection')
                require(not frame.get('triggerEvent'), 'Unbound source frame event')
                sid = frame['spriteId']; key = (collection_name, sid)
                if key not in remap:
                    remap[key] = len(sprites)
                    sprites.append((len(sprites), *decode_sprite(textures[collection_name], collection['spriteDefinitions'][sid])))
                frame['spriteId'] = remap[key]
            clips.append(clip)

    def chain(transform):
        result, seen = [], set()
        while True:
            ident = transform['m_GameObject']['m_PathID']
            require(ident not in seen, 'Cyclic station transform')
            seen.add(ident); result.append(transform)
            parent = transform['m_Father']
            require(parent['m_FileID'] == 0, 'External station transform')
            if not parent['m_PathID']:
                return result
            transform = read(parent['m_PathID'])

    draws, visible = [], 0
    for index, owner in enumerate(OWNERS):
        group = owners[owner]['components']
        go = read(owner)
        transform = read(int(Path(group['Transform'][0]).stem.rsplit('-', 1)[1]))
        transforms = chain(transform)
        origin, px, py = [project(p, transforms) for p in ((0, 0, 0), (1, 0, 0), (0, 1, 0))]
        sprite = json.loads((art / group['tk2dSprite'][0]).read_text())['m_Structure']
        collection_name, _ = part(owner, 'collection')
        key = (collection_name, sprite['_spriteId'])
        # The authored sprite may precede the automatically played clip.
        if key not in remap:
            _, collection = part(owner, 'collection')
            remap[key] = len(sprites)
            sprites.append((len(sprites), *decode_sprite(textures[collection_name], collection['spriteDefinitions'][sprite['_spriteId']])))
        renderer = None
        for ref in go['m_Components']:
            ident = ref['m_Component']['m_PathID']
            name = f'level244-{ident}.json'
            paths = (art / name, source / 'terrain' / name, source / 'visuals' / name)
            if not any(p.is_file() for p in paths):
                continue
            value = read(ident)
            if 'm_SortingLayer' in value:
                require(renderer is None, 'Ambiguous station renderer'); renderer = value
        require(renderer is not None, 'Missing original station renderer')
        if go['m_IsActive'] and renderer['m_Enabled']:
            visible |= 1 << index
        scale, colour = sprite['_scale'], sprite['_color']
        values = [(px[0]-origin[0])*scale['x'], (py[0]-origin[0])*scale['y'], origin[0],
                  (px[1]-origin[1])*scale['x'], (py[1]-origin[1])*scale['y'], origin[1], origin[2],
                  colour['r'], colour['g'], colour['b'], colour['a'], renderer['m_SortingLayer'], renderer['m_SortingOrder'],
                  (px[2]-origin[2])*scale['x'], (py[2]-origin[2])*scale['y'], 0]
        require(all(math.isfinite(v) for v in values), 'Invalid station draw')
        draws.append(struct.pack('<2I16f', remap[key], owner, *values))

    transform = chain(read(673)); polygon_source = read(1121)
    require(polygon_source['m_Enabled'] and polygon_source['m_IsTrigger'] and len(polygon_source['m_Points']['m_Paths']) == 1, 'Changed lever trigger')
    offset = polygon_source['m_Offset']
    polygon = [project((p['m_X']+offset['m_X'], p['m_Y']+offset['m_Y'], 0), transform)[:2]
               for p in polygon_source['m_Points']['m_Paths'][0]]
    require(3 <= len(polygon) <= 16, 'Unsupported lever polygon')
    range_source = read(1128); size, offset = range_source['m_Size'], range_source['m_Offset']
    corners = [project((offset['m_X']+sx*size['m_X']/2, offset['m_Y']+sy*size['m_Y']/2, 0), chain(read(674)))
               for sx in (-1, 1) for sy in (-1, 1)]
    bounds = [min(p[0] for p in corners), min(p[1] for p in corners), max(p[0] for p in corners), max(p[1] for p in corners)]
    dust_data, dust_sprites, dust_clips, dust_records = compile_dust(len(sprites))
    sprites.extend(dust_sprites); clips.extend(dust_clips)
    records, pages = pack_pages(sprites, [(1024, 1024), (1024, 1024)])
    output.mkdir(parents=True, exist_ok=True)
    for index, page in enumerate(pages):
        png = output / f'stag-lever-page-{index}.png'; page.save(png)
        subprocess.run([str(tex3ds), '-f', 'rgba4', '-z', 'none', '-o', str(png.with_suffix('.t3x')), str(png)], check=True)
    (output / 'stag-lever.bin').write_bytes(bank_bytes(clips, list(range(len(sprites))), records, len(pages)))
    data = struct.pack('<4s7f2I', b'SLR1', *bounds, *times, len(polygon), visible)
    data += b''.join(struct.pack('<2f', *p) for p in polygon) + b''.join(draws)
    (output / 'stag-lever-room.bin').write_bytes(data)
    (output / 'stag-lever-fx.bin').write_bytes(dust_data)
    write_audio(output)
    report = dict(scene='RestingGrounds_09', pages=len(pages), texture_bytes=len(pages)*1024*1024*2,
                  sprites=len(sprites), waits=times, range=bounds, polygon=polygon, visible=visible,
                  source_sha256=manifest['sha256'], scene_sha256=source_hashes,
                  approximation='Normal tk2d material; dust uses projected source3D motion and eight-sample curves.', dust=dust_records)
    (output / 'stag-lever-manifest.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps({k: report[k] for k in ('scene','pages','texture_bytes','sprites','waits','visible')}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--art', type=Path, default=ROOT/'_private/minimum-obstacles/resting-station-art')
    parser.add_argument('--source', type=Path, default=ROOT/'_private/research/crossroads-scenes/RestingGrounds_09')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--tex3ds', type=Path, default=Path(TEX3DS_EXE))
    args = parser.parse_args()
    convert(args.art, args.source, args.output, args.tex3ds)
