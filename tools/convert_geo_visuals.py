"""Convert original Geo coin/rock frames to one HKA1 AnimationBank and GRK1 map."""
import converted_files  # Install hardlink copy-on-write before asset writes.
import argparse
import hashlib
import json
import math
import shutil
import struct
import tex3ds_cache
from pathlib import Path
from PIL import Image
from assetripper_client import ROOT
from convert_hero import decode_sprite
from inspect_fsm import decode_fsm
from convert_room_visuals import decode as decode_native_sprite
from tool_paths import TEX3DS as TEX3DS_EXE


def world_transform(transforms):
    position = [0., 0., 0.]
    scale = [1., 1., 1.]
    for t in reversed(transforms):
        if t['m_LocalRotation'] != dict(m_W=1, m_X=0, m_Y=0, m_Z=0):
            raise ValueError('Rotated Geo needs explicit support')
        for i, axis in enumerate(('m_X', 'm_Y', 'm_Z')):
            position[i] += t['m_LocalPosition'][axis] * scale[i]
            scale[i] *= t['m_LocalScale'][axis]
    return position, scale


def downsample(sprite, scale=0.5):
    size = (math.ceil(sprite.width*scale), math.ceil(sprite.height*scale))
    return sprite.convert('RGBa').resize(size, Image.Resampling.BOX).convert('RGBA')


def cocoon_fx(groups, read):
    """Serialize the original instance/prefab values, without runtime guesses."""
    by_kind = {g['kind']: g for g in groups}
    def component(kind, name, script=False):
        value = read(by_kind[kind]['parts'][name][0])
        return value['m_Structure'] if script else value
    control = component('cocoon', 'HealthCocoon', True)
    cap = component('cap', 'Rigidbody2D')
    bounce = component('cap', 'ObjectBounce', True)
    spatter = component('spatter', 'SpatterOrange', True)
    glob = component('glob', 'GlobControl', True)
    position, scale = world_transform([read(n) for n in
        by_kind['cap']['parts']['Transform'] + by_kind['cap']['ancestors']])
    root, _ = world_transform([read(n) for n in
        by_kind['cocoon']['parts']['Transform'] + by_kind['cocoon']['ancestors']])
    # The cached exporter fetched this PolygonCollider2D but its old type census
    # omitted it. Resolve the actual component references rather than a fixed ID.
    cap_go = by_kind['cap']['parts']['GameObject'][0]
    collection = cap_go.rsplit('-', 1)[0]
    components = [read(f'{collection}-{c["m_Component"]["m_PathID"]}.json')
                  for c in read(cap_go)['m_Components']]
    polygons = [c for c in components if 'm_Points' in c]
    if len(polygons) != 1 or len(polygons[0]['m_Points']['m_Paths']) != 1:
        raise ValueError('Cap needs one source polygon for inertia')
    vertices = [(v['m_X']*scale[0], v['m_Y']*scale[1])
                for v in polygons[0]['m_Points']['m_Paths'][0]]
    area2 = cx = cy = moment = 0.
    for (x,y), (u,v) in zip(vertices, vertices[1:]+vertices[:1]):
        cross = x*v-u*y
        area2 += cross
        cx += (x+u)*cross
        cy += (y+v)*cross
        moment += cross*(x*x+x*u+u*u+y*y+y*v+v*v)
    if abs(area2) < 1e-8:
        raise ValueError('Degenerate source cap polygon')
    inertia = cap['m_Mass']*(moment/(6*area2)-(cx/(3*area2))**2-(cy/(3*area2))**2)
    gravity = json.loads((ROOT/'_private/research/assets/Physics2DSettings.json').read_text())['m_Gravity']['m_Y']
    flings = control['flingPrefabs'][:2]
    if [(f['minAmount'], f['maxAmount']) for f in flings] != [(50, 50), (8, 10)]:
        raise ValueError('Cocoon fling counts exceed the partitioned 60-slot pool')
    data = b''
    for f in flings:
        data += struct.pack('<2I6f', f['minAmount'], f['maxAmount'],
                            f['originVariation']['x'], f['originVariation']['y'],
                            f['minSpeed'], f['maxSpeed'], f['minAngle'], f['maxAngle'])
    values = [control['capHitForce'], cap['m_Mass'], cap['m_GravityScale'],
              component('glob', 'Rigidbody2D')['m_GravityScale'],
              component('spatter', 'Rigidbody2D')['m_GravityScale'],
              bounce['bounceFactor'], bounce['speedThreshold'],
              component('cap', 'SpinSelf', True)['spinFactor'], cap['m_AngularDamping'],
              spatter['scaleModifierMin'], spatter['scaleModifierMax'],
              spatter['splashScaleMin'], spatter['splashScaleMax'],
              glob['minScale'], glob['maxScale'],
              position[0]-root[0], position[1]-root[1], *scale[:2], position[2]-root[2], gravity, inertia]
    if not all(math.isfinite(v) for v in values) or cap['m_Mass'] <= 0:
        raise ValueError('Invalid source cocoon physics')
    data += struct.pack('<22f', *values)
    return data, dict(flings=flings, values=values)


def pack_sprites(sprites, page, page_index, records, *, shared_gutters=False):
    """Pack non-rotated sprites with one transparent texel on each side."""
    # Adjacent sprites may share their transparent border. Reserve the outer
    # right/bottom texel too, so even sprites at a page edge keep a full gutter.
    margin = int(shared_gutters)
    free = [(0, 0, page.width-margin, page.height-margin)]
    ordered = sorted(sprites, key=lambda item: (-max(item[1].size), -item[1].width*item[1].height, item[0]))
    for key, bitmap, geometry in ordered:
        w, h = bitmap.width+2-margin, bitmap.height+2-margin
        candidates = []
        for index, (x, y, fw, fh) in enumerate(free):
            if w <= fw and h <= fh:
                candidates.append(((min(fw-w, fh-h), max(fw-w, fh-h), fw*fh-w*h), index, x, y))
        if not candidates:
            raise ValueError('Geo and cocoon artwork exceed the two-page texture budget')
        _, _, x, y = min(candidates)
        placed = (x, y, w, h)
        page.paste(bitmap, (x+1, y+1))
        records[key] = (page_index, x+1, y+1, bitmap.width, bitmap.height, *geometry)
        next_free = []
        px, py, pw, ph = placed
        for fx, fy, fw, fh in free:
            if px >= fx+fw or px+pw <= fx or py >= fy+fh or py+ph <= fy:
                next_free.append((fx, fy, fw, fh))
                continue
            if px > fx: next_free.append((fx, fy, px-fx, fh))
            if px+pw < fx+fw: next_free.append((px+pw, fy, fx+fw-(px+pw), fh))
            if py > fy: next_free.append((fx, fy, fw, py-fy))
            if py+ph < fy+fh: next_free.append((fx, py+ph, fw, fy+fh-(py+ph)))
        free = [rect for i, rect in enumerate(next_free)
                if not any(i != j and rect[0] >= other[0] and rect[1] >= other[1]
                           and rect[0]+rect[2] <= other[0]+other[2]
                           and rect[1]+rect[3] <= other[1]+other[3]
                           for j, other in enumerate(next_free))]


def collider_union(boxes):
    bounds = []
    for box in boxes:
        enabled = box['m_Enabled']
        if enabled not in (0, 1):
            raise ValueError('Unsupported BoxCollider2D enabled state')
        if not enabled:
            continue
        if box['m_AutoTiling'] or box['m_EdgeRadius'] != 0:
            raise ValueError('Unsupported BoxCollider2D geometry')
        offset, size = box['m_Offset'], box['m_Size']
        values = (offset['m_X'], offset['m_Y'], size['m_X'], size['m_Y'])
        if not all(math.isfinite(value) for value in values) or size['m_X'] <= 0 or size['m_Y'] <= 0:
            raise ValueError('Invalid BoxCollider2D rectangle')
        bounds.append((offset['m_X'] - size['m_X'] / 2,
                       offset['m_Y'] - size['m_Y'] / 2,
                       offset['m_X'] + size['m_X'] / 2,
                       offset['m_Y'] + size['m_Y'] / 2))
    if not bounds:
        raise ValueError('Rock has no enabled BoxCollider2D')
    return [min(rect[0] for rect in bounds), min(rect[1] for rect in bounds),
            max(rect[2] for rect in bounds), max(rect[3] for rect in bounds)]


def chip_data(mapping, read, chip, chip_read, gravity):
    profiles = None
    for group in (g for g in mapping['groups'] if g['kind'] == 'rock'):
        fsm = decode_fsm(read(group['parts']['PlayMakerFSM'][0])['m_Structure']['fsm'])
        rows = []
        for name in ('Hit Right', 'Hit Left', 'Hit Down', 'Destroy'):
            state = next(s for s in fsm['states'] if s['name'] == name)
            flings = [a['parameters'] for a in state['actions'] if a['enabled'] and
                      a['type'].endswith('.FlingObjectsFromGlobalPool') and
                      a['parameters']['gameObject']['value']['m_PathID'] == chip['prefab']]
            if len(flings) != 1:
                raise ValueError('Expected one source chip fling per rock state')
            p = flings[0]
            rows.append([p[k]['value'] for k in ('spawnMin', 'spawnMax', 'speedMin', 'speedMax',
                                                'angleMin', 'angleMax', 'originVariationX', 'originVariationY')])
        if profiles is not None and profiles != rows:
            raise ValueError('Geo rock owners have different chip profiles')
        profiles = rows
    if len({tuple(row[6:]) for row in profiles}) != 1:
        raise ValueError('Chip origin variation differs between states')
    parts = chip['parts']
    debris = chip_read(parts['debris'])['m_Structure']
    body = chip_read(parts['body'])
    box = chip_read(parts['collider'])
    bounce = chip_read(parts['bounce'])['m_Structure']
    finishing = chip_read(parts['finishing'])['m_Structure']
    if body['m_LinearDamping'] != 0 or finishing['persistOffScreen'] or finishing['conclusion'] != 1:
        raise ValueError('Unsupported stone chip lifecycle')
    collection = chip_read(chip['collection'])['m_Structure']
    ids = [next(i for i, s in enumerate(collection['spriteDefinitions']) if s['name'] == name)
           for name in debris['randomSpriteIds']]
    # .009 depth variation is DebrisParticle.OnEnable's C# constant.
    common = [*profiles[0][6:], debris['scaleMin'], debris['scaleMax'], debris['blackChance'],
              debris['startZ'], .009, body['m_AngularDamping'], gravity * body['m_GravityScale'], body['m_Mass'],
              box['m_Size']['m_X'], box['m_Size']['m_Y'], box['m_Offset']['m_Y'],
              bounce['bounceFactor'], bounce['speedThreshold'], finishing['waitDuration'], finishing['shrinkDuration']]
    return dict(profiles=[row[:6] for row in profiles], common=common, sprite_ids=ids)


def convert(source, output, romfs, tex3ds, chip_source=None):
    for path in (output, romfs):
        if not path.absolute().is_relative_to(ROOT/'_private'):
            raise ValueError('Converted original resources must remain private')
        path.mkdir(parents=True, exist_ok=True)
    mapping = json.loads((source/'geo-visuals-map.json').read_text())
    chip_source = chip_source or ROOT/'_private/converted/geo-chip-source'
    chip = json.loads((chip_source/'geo-chips-map.json').read_text())
    def chip_read(name): return json.loads((chip_source/name).read_text())
    for name, expected in chip['source_sha256'].items():
        if hashlib.sha256((chip_source/name).read_bytes()).hexdigest() != expected:
            raise ValueError('Changed stone chip source: '+name)
    def read(name): return json.loads((source/name).read_text())
    for name, expected in mapping['source_sha256'].items():
        if hashlib.sha256((source/name).read_bytes()).hexdigest() != expected:
            raise ValueError('Changed original source: '+name)
    clips = []
    collections = {}
    atlases = {}
    sprite_scales = {}
    sprite_kinds = {}
    sprite_tints = {}
    native_sprites = {}
    seen_clips = set()
    gravity_path = ROOT/'_private/research/assets/Physics2DSettings.json'
    gravity_raw = gravity_path.read_bytes()
    gravity = json.loads(gravity_raw)['m_Gravity']
    if gravity['m_X'] != 0 or gravity['m_Y'] > 0:
        raise ValueError('Unsupported stone chip gravity direction')
    chips = chip_data(mapping, read, chip, chip_read, gravity['m_Y'])
    collection = chip_read(chip['collection'])['m_Structure']
    texture = Image.open(chip_source/chip['texture']).convert('RGBA')
    chip_keys = []
    for black in (False, True):
        for index in chips['sprite_ids']:
            key = (chip['collection'] + ('#black' if black else ''), index)
            native_sprites[key] = decode_sprite(texture, collection['spriteDefinitions'][index])
            sprite_scales[key] = 1
            sprite_kinds[key] = 'chip'
            sprite_tints[key] = (0, 0, 0, 1) if black else (1, 1, 1, 1)
            chip_keys.append(key)
    clips.append(dict(name='Rock Chips', fps=1, wrapMode=0, loopStart=0, sprite_keys=chip_keys))
    for group in mapping['groups']:
        # Cocoon and Geo hits draw these pooled effects from DecorEffects.
        # Keeping another copy in this bank wastes page space and clip records.
        if group['kind'] in ('slash', 'nail'):
            continue
        if group['kind'] in ('cap', 'spatter', 'heal'):
            native_keys = []
            for native in group['native_frames']:
                key = (native['sprite'], -1)
                sprite, geometry = decode_native_sprite(Image.open(source/native['texture']).convert('RGBA'),
                                                        read(native['sprite']))
                native_sprites[key] = (sprite, geometry)
                sprite_scales[key] = 1.0/3.0
                sprite_kinds[key] = group['kind']
                native_keys.append(key)
            if group['kind'] == 'heal':
                clips.append(dict(name='Spatter Blue Heal', fps=1.0, wrapMode=0, loopStart=0,
                                  sprite_keys=native_keys[:1]))
            elif group['kind'] == 'cap':
                clips.append(dict(name='Cocoon Cap', fps=1.0, wrapMode=0, loopStart=0,
                                  sprite_keys=native_keys[:1]))
            else:
                if len(native_keys) != 7:
                    raise ValueError('Spatter Blue source must provide its projectile and six impact frames')
                clips.append(dict(name='Spatter Blue', fps=1.0, wrapMode=0, loopStart=0,
                                  sprite_keys=native_keys[:1]))
                clips.append(dict(name='Spatter Blue Impact', fps=30.0, wrapMode=2, loopStart=0,
                                  sprite_keys=native_keys[1:]))
            continue
        library = {c['name']: c for c in read(group['library'])['m_Structure']['clips']}
        color = read(group['parts']['tk2dSprite'][0])['m_Structure']['_color']
        tint = tuple(color[channel] for channel in ('r', 'g', 'b', 'a'))
        if any(not math.isfinite(value) or not 0 <= value <= 1 for value in tint):
            raise ValueError('Unsupported Geo sprite tint')
        names = list(group['clips'])
        # Same verified tk2d libraries/collections as the cocoon and its splat.
        if group['kind'] == 'cocoon':
            names += ['Scuttler Land', 'Scuttler Run']
        if group['kind'] == 'splat':
            names += ['Splat', 'Health Get']
        for name in names:
            if (group['library'], name) in seen_clips:
                continue
            seen_clips.add((group['library'], name))
            clip = dict(library[name])
            if clip['wrapMode'] not in (0, 1, 2, 6) or clip['fps'] <= 0:
                raise ValueError('Unsupported Geo timing')
            frames = []
            for frame in clip['frames']:
                if frame['triggerEvent']:
                    raise ValueError('Geo frame event needs runtime support')
                ref = frame['spriteCollection']
                ck = group['sprite_collections'][f'{ref["m_FileID"]}:{ref["m_PathID"]}']
                frames.append((ck, frame['spriteId']))
                sprite_scales[(ck, frame['spriteId'])] = (1.0/3.0) if group['kind'] in ('cocoon', 'splat', 'glob') else 1.0
                sprite_kinds[(ck, frame['spriteId'])] = group['kind']
                key = (ck, frame['spriteId'])
                if key in sprite_tints and sprite_tints[key] != tint:
                    raise ValueError('Shared Geo sprite needs distinct tinted records')
                sprite_tints[key] = tint
                if ck not in collections:
                    collections[ck] = read(ck)['m_Structure']
                    atlases[ck] = Image.open(source/group['textures'][ck]).convert('RGBA')
            clip['sprite_keys'] = frames
            clips.append(clip)
    if len({c['name'] for c in clips}) != len(clips):
        raise ValueError('AnimationBank clip names must be unique')
    ids = sorted({k for c in clips for k in c['sprite_keys']})
    sprites = []
    for key in ids:
        if key in native_sprites:
            bitmap, geometry = native_sprites[key]
        else:
            bitmap, geometry = decode_sprite(atlases[key[0]], collections[key[0]]['spriteDefinitions'][key[1]])
        if sprite_scales[key] < 1.0:
            bitmap = downsample(bitmap, sprite_scales[key])
        # tk2d multiplies each renderer's source color after texture sampling.
        # Bake static colors (notably the cyan cocoon Splat2) into this atlas.
        tint = sprite_tints.get(key, (1, 1, 1, 1))
        bitmap = Image.merge('RGBA', tuple(channel.point([round(v*factor) for v in range(256)])
                                          for channel, factor in zip(bitmap.split(), tint)))
        sprites.append((key, bitmap, geometry))
    sprites.sort(key=lambda s: (-s[1].height, -s[1].width, s[0]))
    page_size = 512
    pages = [Image.new('RGBA', (page_size, page_size)), Image.new('RGBA', (page_size, page_size))]
    records = {}
    for page_index, kinds in ((0, {'coin', 'rock', 'chip'}),
                              (1, {'cocoon', 'splat', 'glob', 'cap', 'spatter', 'heal'})):
        page_sprites = [(key, bitmap, geometry) for key, bitmap, geometry in sprites
                        if sprite_kinds[key] in kinds]
        if page_index == 1:
            pack_sprites(page_sprites, pages[page_index], page_index, records)
            continue
        x = y = row = 0
        page = pages[page_index]
        for key, bitmap, geometry in page_sprites:
            w, h = bitmap.size
            if x+w+2 > page_size:
                x = 0; y += row; row = 0
            if w+2 > page_size or h+2 > page_size:
                raise ValueError('Geo sprite exceeds the 512x512 page limit')
            if y+h+2 > page_size:
                raise ValueError('Geo and cocoon artwork exceed the two-page texture budget')
            page.paste(bitmap, (x+1, y+1))
            records[key] = (page_index, x+1, y+1, w, h, *geometry)
            x += w+2
            row = max(row, h+2)
    total = sum(len(c['sprite_keys']) for c in clips)
    with (output/'geo-visual.bin').open('wb') as out:
        out.write(struct.pack('<4s4I', b'HKA1', len(pages), len(ids), len(clips), total))
        for key in ids:
            out.write(struct.pack('<5I4f', *records[key]))
        first = 0
        for c in clips:
            if len(c['name'].encode()) >= 48:
                raise ValueError('Clip name too long')
            out.write(struct.pack('<48sf4I', c['name'].encode(), c['fps'], c['wrapMode'], c['loopStart'], first, len(c['sprite_keys'])))
            first += len(c['sprite_keys'])
        for c in clips:
            for key in c['sprite_keys']:
                out.write(struct.pack('<I', ids.index(key)))
    rocks = []
    coin = None
    for group in (g for g in mapping['groups'] if g['kind'] in ('coin', 'rock')):
        parts = group['parts']
        position, scale = world_transform([read(n) for n in parts['Transform']+group['ancestors']])
        sprite = read(parts['tk2dSprite'][0])['m_Structure']
        if sprite['_scale'] != dict(x=1, y=1, z=1):
            raise ValueError('Scaled tk2d sprite needs separate collision/render transforms')
        if group['kind'] == 'coin':
            coin = dict(scale=scale, source_position=position)
            continue
        animator = read(parts['tk2dSpriteAnimator'][0])['m_Structure']
        library = read(group['library'])['m_Structure']['clips']
        idle = library[animator['defaultClipId']]['name']
        variant = int(idle.removeprefix('Idle '))
        if library[animator['defaultClipId']]['frames'][0]['spriteId'] != sprite['_spriteId']:
            raise ValueError('Rock default sprite differs from idle clip')
        rect = collider_union([read(n) for n in parts['BoxCollider2D']])
        fsm = decode_fsm(read(parts['PlayMakerFSM'][0])['m_Structure']['fsm'])
        hits = next(v['value'] for v in fsm['variables']['intVariables'] if v['name'] == 'Hits')
        rocks.append(dict(owner=group['owner'], variant=variant, hit_count=hits, position=position,
                          scale=scale[:2], collider=rect, idle_clip=idle,
                          gleam_clip=f'Gleam {variant}', broken_clip=f'Broken {variant}'))
    with (output/'geo-rocks.bin').open('wb') as out:
        out.write(struct.pack('<4sI', b'GRK1', len(rocks)))
        for r in rocks:
            values = [*r['position'], *r['scale'], *r['collider']]
            if not all(math.isfinite(v) for v in values):
                raise ValueError('Nonfinite rock geometry')
            out.write(struct.pack('<3I9f48s48s48s', r['owner'], r['variant'], r['hit_count'], *values,
                                  *(r[n].encode() for n in ('idle_clip', 'gleam_clip', 'broken_clip'))))
        out.write(b'GCH2')
        for row in chips['profiles']:
            out.write(struct.pack('<2I4f', *row))
        out.write(struct.pack('<17f', *chips['common']))
    cocoon = None
    splat = None
    splat_group = next((g for g in mapping['groups'] if g['kind'] == 'splat'), None)
    if splat_group is None:
        raise ValueError('Health Cocoon needs its Splat Effect child transform')
    splat_position, splat_scale = world_transform(
        [read(n) for n in splat_group['parts']['Transform']+splat_group['ancestors']])
    splat = dict(position=splat_position, scale=splat_scale)
    for group in (g for g in mapping['groups'] if g['kind'] == 'cocoon'):
        transforms = [read(n) for n in group['parts']['Transform']+group['ancestors']]
        position, scale = world_transform(transforms)
        triggers = [read(n) for n in group['parts'].get('BoxCollider2D', [])]
        triggers = [box for box in triggers if box.get('m_Enabled') and box.get('m_IsTrigger')]
        if not triggers:
            raise ValueError('Health Cocoon has no enabled Nail Attack trigger')
        bounds = []
        for box in triggers:
            offset, size = box['m_Offset'], box['m_Size']
            x0 = position[0]+scale[0]*(offset['m_X']-size['m_X']/2)
            x1 = position[0]+scale[0]*(offset['m_X']+size['m_X']/2)
            y0 = position[1]+scale[1]*(offset['m_Y']-size['m_Y']/2)
            y1 = position[1]+scale[1]*(offset['m_Y']+size['m_Y']/2)
            bounds.append((min(x0,x1),min(y0,y1),max(x0,x1),max(y0,y1)))
        world_bounds = [min(b[0] for b in bounds),min(b[1] for b in bounds),
                        max(b[2] for b in bounds),max(b[3] for b in bounds)]
        control = read(group['parts']['HealthCocoon'][0])['m_Structure']
        disabled = [ref['m_PathID'] for ref in control['disableColliders'] if ref['m_FileID'] == 0]
        if len(disabled) != 2 or not all(disabled) or len(set(disabled)) != 2:
            raise ValueError('Health Cocoon needs its two source disableColliders')
        cocoon = dict(owner=group['owner'], position=position, scale=scale,
                      nail_trigger_bounds=world_bounds, disabled_colliders=disabled)
        fx_data, cocoon['fx'] = cocoon_fx(mapping['groups'], read)
        (output/'health-cocoon.bin').write_bytes(struct.pack(
            '<4sI16f2I', b'HCC3', group['owner'], *position, *scale, *world_bounds,
            *splat_position, *splat_scale, *disabled) + fx_data)
    page_names = []
    for index, page in enumerate(pages):
        png = output/f'geo-page-{index}.png'
        t3x = output/f'geo-page-{index}.t3x'
        page.save(png)
        tex3ds_cache.run([str(tex3ds), '-f', 'rgba8', '-z', 'none', '-o', str(t3x), str(png)], check=True)
        page_names.append(t3x.name)
    for name in ('geo-visual.bin', 'geo-rocks.bin', 'health-cocoon.bin', *page_names):
        shutil.copyfile(output/name, romfs/name)
    report = dict(source_sha256=mapping['source_sha256'], chip_source_sha256=chip['source_sha256'], coin=coin, rocks=rocks, chips=chips,
                  physics_source_sha256=hashlib.sha256(gravity_raw).hexdigest(),
                  cocoon=cocoon, splat=splat,
                  clips=[dict(name=c['name'], fps=c['fps'], wrapMode=c['wrapMode'], loopStart=c['loopStart'], frames=len(c['sprite_keys']), sprite_ids=[list(k) for k in c['sprite_keys']]) for c in clips],
                  sprite_records=[dict(collection=k[0], sprite_id=k[1], record=records[k]) for k in ids],
                  sprites=len(ids), frames=total, page_count=len(pages),
                  texture_page_bytes=page_size*page_size*4,
                  texture_bytes=len(pages)*page_size*page_size*4,
                  raster_scale={'cocoon': 1.0/3.0, 'splat': 1.0/3.0, 'glob': 1.0/3.0,
                                'cap': 1.0/3.0, 'spatter': 1.0/3.0})
    (output/'manifest.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps({k:report[k] for k in ('sprites', 'frames', 'page_count', 'texture_bytes', 'coin', 'rocks', 'cocoon')}))


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, default=ROOT/'_private/research/geo-visuals')
    p.add_argument('--output', type=Path, default=ROOT/'_private/converted/geo-visuals')
    p.add_argument('--romfs', type=Path, default=ROOT/'_private/converted/hero/romfs')
    p.add_argument('--tex3ds', type=Path, default=Path(TEX3DS_EXE))
    a = p.parse_args()
    convert(a.source, a.output, a.romfs, a.tex3ds)
