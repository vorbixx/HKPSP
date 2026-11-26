"""Pack source HUD/limiter art and LCD text into 768 KiB of RGBA8/LA8 pages."""
import converted_files  # Install hardlink copy-on-write before asset writes.
import argparse
import csv
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
from convert_tutorial_prompts import append_prompts
from convert_opening_credits import convert as convert_credits, append_labels as append_credits
from convert_lore_tablets import append_lore
from convert_area_title import append_area_title
from text_raster import lcd_text, mixed_pages, texture_bytes, source_detail
from top_screen_ui_layout import LAYOUT
from convert_room_visuals import decode as decode_native_sprite
from convert_decor_effects import limits
from tool_paths import TEX3DS as TEX3DS_EXE

SCALE = .25
# Keep the fixed atlas allocation. Logical bounds, not this source raster,
# determine HUD placement/size. Retain detail for the final approved view scale.
VITALS_SCALE = LAYOUT['vitalsScale']
COLOR_PAGE_SIZE = (512, 256)
TEXT_PAGE_SIZE = (512, 256)
CLIPS = ('Health Empty', 'Health Idle', 'Health Break', 'Health Refill', 'Health Appear',
         'Blue Appear', 'Blue Idle', 'Blue Break', 'Coin Appear', 'Coin Get', 'Coin Idle', 'Coin Get Loop', 'HUD Frame Idle',
         'Soul Limiter Idle', 'HUD Frame Cracked')


def reduced(bitmap, geometry=None, draw_scale=1):
    if geometry is not None:
        return source_detail(bitmap, geometry, 16*VITALS_SCALE*draw_scale, SCALE)
    return bitmap.convert('RGBa').resize(tuple(max(1, round(v * SCALE)) for v in bitmap.size),
                                       Image.Resampling.BOX).convert('RGBA')


def append_feedback(static, clips, sprites, original, by_name):
    source = ROOT / '_private/research/hud-feedback'
    mapping = json.loads((source / 'feedback-map.json').read_text())
    for name, expected in mapping['source_sha256'].items():
        if hashlib.sha256((source / name).read_bytes()).hexdigest() != expected:
            raise ValueError('Changed HUD feedback source: ' + name)
    def read(name): return json.loads((source / name).read_text())
    def transform(owner): return read(mapping['objects'][str(owner)]['Transform'][0])
    anchor = json.loads((ROOT / '_private/research/hud/resources.assets-10329.json').read_text())['m_LocalPosition']
    def geometry(bounds, owner, parent=None):
        t = transform(owner); pos, scale = t['m_LocalPosition'], t['m_LocalScale']
        x, y, sx, sy = pos['m_X'], pos['m_Y'], scale['m_X'], scale['m_Y']
        if parent:
            t = transform(parent); p, s = t['m_LocalPosition'], t['m_LocalScale']
            x, y = p['m_X']+x*s['m_X'], p['m_Y']+y*s['m_Y']
            sx, sy = sx*s['m_X'], sy*s['m_Y']
        l, b, r, t = bounds
        return (l*sx+x-anchor['m_X'], b*sy+y-anchor['m_Y'],
                r*sx+x-anchor['m_X'], t*sy+y-anchor['m_Y'])
    for name, owner, parent in [('Soul Gain Flash', 6389, None), ('Soul Full Glow', 4258, None),
                                 ('Soul Full Rim', 8326, 4258)]:
        filename = mapping['objects'][str(owner)]['SpriteRenderer'][0]
        entry = mapping['visuals'][filename]
        if mapping['materials'][entry['materials'][0]]['shader_name'] != 'Sprites/Default':
            raise ValueError('Unexpected Soul feedback blend')
        bitmap, bounds = decode_native_sprite(Image.open(source / mapping['textures'][entry['texture']]).convert('RGBA'), read(entry['sprite']))
        # Prefilter the smooth flash more heavily than the detailed rim. The
        # authored geometry remains unchanged inside the fixed HUD allocation.
        factor = .125 if owner==6389 else SCALE
        bitmap = bitmap.convert('RGBa').resize(tuple(max(1,round(n*factor)) for n in bitmap.size), Image.Resampling.BOX).convert('RGBA')
        static(name, ('feedback', owner), bitmap, geometry(bounds, owner, parent))
    clip = dict(by_name['Soul Burst']); clip['keys'] = []
    for f in clip['frames']:
        key = original((f['spriteCollection']['m_PathID'], f['spriteId']))
        bitmap, bounds = sprites.pop(key)
        # These five frames belong only to Soul Burst. Filter at 12 texels per
        # source unit, retaining all frames and their doubled source transform.
        bitmap = bitmap.convert('RGBa').resize(tuple(max(1,round(n*.75)) for n in bitmap.size), Image.Resampling.BOX).convert('RGBA')
        new_key = ('burst', f['spriteId']); sprites[new_key] = bitmap, geometry(bounds, 4449)
        clip['keys'].append(new_key)
    clips.append(clip)
    particle_material = mapping['materials']['resources.assets-155.json']
    if particle_material['shader_name'] != 'Legacy Shaders/Particles/Additive (Soft)':
        raise ValueError('Unexpected refill blend')
    bitmap = Image.open(source / mapping['textures'][particle_material['textures']['_MainTex']]).convert('RGBA')
    # Maximum authored size is eight LCD pixels; retain sixteen texels per unit.
    bitmap = bitmap.convert('RGBa').resize((16,16), Image.Resampling.BOX).convert('RGBA')
    static('Health Refill Particle', ('feedback', 'particle'), bitmap, (-.5,-.5,.5,.5))
    fsms = json.loads((source / 'fsm-decoded.json').read_text())
    flash = next(x['fsm'] for x in fsms if x['file']=='resources.assets-23542.json')
    ease = next(a['parameters'] for s in flash['states'] if s['name']=='Flash'
                for a in s['actions'] if a['type'].endswith('.EaseColor'))
    pulse = read('resources.assets-27170.json')['m_Structure']
    alpha = ease['fromValue']['value']['a']
    values = dict(soulFlashSeconds=ease['time']['value'], soulFlashAlpha=alpha,
                  soulPulseSeconds=pulse['fadeDuration'], soulBurstSeconds=len(clip['keys'])/clip['fps'])
    profiles = []
    for owner in (5126,3923,5166):
        parts = mapping['objects'][str(owner)]; d = read(parts['ParticleSystem'][0])
        initial, shape, clamp = d['m_InitialModule'], d['m_ShapeModule'], d['m_ClampVelocityModule']
        if shape['m_Type'] != (10 if owner==5166 else 12) or d['m_Looping'] or d['m_UseUnscaledTime']:
            raise ValueError('Unsupported ordinary HUD refill emitter')
        pos, scale = transform(owner)['m_LocalPosition'], (1 if owner==5166 else transform(5625)['m_LocalScale']['m_X'])
        shape_scale = scale*transform(owner)['m_LocalScale']['m_X']
        rate = limits(d['m_EmissionModule']['m_RateOverTime'])[0]
        profiles.append(dict(owner=owner, system=parts['ParticleSystem'][0], count=round(rate*d['m_LengthInSec']),
            values=[pos['m_X']*scale-(anchor['m_X'] if owner==5166 else 0),
                    pos['m_Y']*scale-(anchor['m_Y'] if owner==5166 else 0),1 if owner!=3923 else -1,
                    shape['m_RadiusParameter']['m_Value']*shape_scale,rate,d['m_LengthInSec'],
                    *limits(initial['m_StartLifetime']), *limits(initial['m_StartSize']),
                    *limits(initial['m_StartSpeed']),*limits(clamp['m_Magnitude']),clamp['m_Dampen'] if clamp['m_Enabled'] else 0,
                    *(limits(d['m_ForceModule']['m_Y']) if d['m_ForceModule']['m_Enabled'] else (0,0)),
                    limits(d['m_StartDelay_MinMaxCurve'])[0]]))
    curve = read(profiles[0]['system'])['m_SizeModule']['m_Curve']
    code = ''.join(f'inline constexpr float {k} = {v:.9f}f;\n' for k,v in values.items())
    code += 'struct HudRefillEmitter { float x,y,direction,radius,rate,seconds,lifeMin,lifeMax,sizeMin,sizeMax,speedMin,speedMax,limitMin,limitMax,dampen,forceMin,forceMax,delay; };\n'
    code += 'inline constexpr HudRefillEmitter refillEmitters[] = {\n' + ''.join(
        '{'+','.join(f'{v:.9f}f' for v in p['values'])+'},\n' for p in profiles) + '};\n'
    for index, name in enumerate(('refillSizeMin','refillSizeMax')):
        code += 'inline constexpr float '+name+'[] = {'+','.join(f'{limits(curve,i/31)[index]:.9f}f' for i in range(32))+'};\n'
    events = [
        dict(event='MP GAIN below maxMP',owner=6389,fsm=23542,controller=21294,sprite=1987,
             eligibility='Setting=false; FLASH ignored while Flash runs',blend='Sprites/Default',clock='scaled easeOutSine .15s, alpha 180/255 -> 0'),
        dict(event='Can Heal 2',owner=5166,system=14492,controller=21294,
             eligibility='not Focusing, MP >= focusMP_amount, Can Focus Anim=false, Setting=false',blend='Additive (Soft)',clock='scaled .11s delay + .2s emission + .8..1.2s lifetime'),
        dict(event='MP Full',owner=4258,sprite=3032,controller=21294,
             eligibility='Orb Filled=false, not soulLimited/bound; disabled by drain/loss',blend='Sprites/Default',clock='steady'),
        dict(event='Orb Full enabled',owner=8326,sprite=2777,component=27170,parent=4258,
             eligibility='parent Orb Full active',blend='Sprites/Default',clock='scaled .5s up / .5s down'),
        dict(event='MP Full',owner=4449,animator=22896,library=20665,clip=16,
             eligibility='Orb Filled=false and Setting=false',blend='tk2d alpha',clock='5 frames at 20fps, deactivate at .25s'),
        dict(event='health_display Heal?',controller=24263,prefab=5625,owners=[5126,3923],systems=[14559,14580],
             eligibility='ordinary empty mask becomes white; Blue Appear/Max Up excluded',blend='Additive (Soft)',clock='scaled .1s emission + .4..0.8s lifetime')]
    return dict(source_sha256=mapping['source_sha256'], clocks=values, emitters=profiles, events=events,
                artwork_filter='flash .125 native pixels; burst .1875; other feedback .25; authored geometry retained',
                particles_per_refill=sum(p['count'] for p in profiles[:2]), particles_per_threshold=profiles[2]['count'],
                peak_slots=120, peak_quads=124), code


def append_limiter(static, clips, sprites, original, by_name):
    source = ROOT / '_private/research/hud-limiter'
    mapping = json.loads((source / 'limiter-map.json').read_text())
    for name, expected in mapping['source_sha256'].items():
        if hashlib.sha256((source / name).read_bytes()).hexdigest() != expected:
            raise ValueError('Changed Soul Limiter source: ' + name)
    def read(name): return json.loads((source / name).read_text())
    def transform(owner): return read(mapping['objects'][str(owner)]['Transform'][0])
    anchor = json.loads((ROOT / '_private/research/hud/resources.assets-10329.json').read_text())['m_LocalPosition']
    burst = transform(6493)['m_LocalPosition']
    clip = dict(by_name['Soul Burst']); clip.update(name='Soul Limiter Burst', keys=[])
    for f in clip['frames']:
        _, bounds = sprites.pop(original((f['spriteCollection']['m_PathID'], f['spriteId'])))
        bitmap = sprites[('burst', f['spriteId'])][0]
        key = ('limiter-burst', f['spriteId'])
        x, y = burst['m_X']-anchor['m_X'], burst['m_Y']-anchor['m_Y']
        sprites[key] = bitmap, (bounds[0]+x, bounds[1]+y, bounds[2]+x, bounds[3]+y)
        clip['keys'].append(key)
    clips.append(clip)
    emitter = read(mapping['objects']['4177']['ParticleSystem'][0])
    initial, shape, clamp = (emitter[k] for k in ('m_InitialModule','m_ShapeModule','m_ClampVelocityModule'))
    t = transform(4177)
    if shape['m_Type'] != 4 or emitter['m_Looping'] or emitter['m_ScalingMode'] != 2:
        raise ValueError('Unsupported Soul Orb Break emitter')
    material = mapping['materials']['resources.assets-133.json']
    if material['shader_name'] != 'Sprites/Default':
        raise ValueError('Unexpected limiter fragment shader')
    bitmap = Image.open(source / mapping['textures'][material['textures']['_MainTex']]).convert('RGBA')
    uv = emitter['m_UVModule']
    if (bitmap.size,uv['m_Enabled'],uv['m_TilesX'],uv['m_TilesY'],uv['m_AnimationType'],uv['m_RowMode']) != ((51,153),True,1,3,1,1):
        raise ValueError('Changed limiter random-row shard sheet')
    fragment_keys = []
    for row in range(3):
        key = ('limiter-fragment',row)
        tile = bitmap.crop((0,row*51,51,(row+1)*51))
        tile = tile.convert('RGBa').resize((16,16), Image.Resampling.BOX).convert('RGBA')
        sprites[key] = tile, (-.5,-.5,.5,.5); fragment_keys.append(key)
    clips.append(dict(name='Soul Limiter Fragment',fps=30,wrapMode=6,loopStart=0,keys=fragment_keys))
    values = dict(limiterBurstSeconds=len(clip['frames'])/clip['fps'],
        limiterX=t['m_LocalPosition']['m_X']-anchor['m_X'], limiterY=t['m_LocalPosition']['m_Y']-anchor['m_Y'],
        limiterRate=limits(emitter['m_EmissionModule']['m_RateOverTime'])[0], limiterEmissionSeconds=emitter['m_LengthInSec'],
        limiterRadius=shape['m_RadiusParameter']['m_Value'], limiterAngle=math.radians(shape['m_Angle']),
        limiterGravity=-9.81*limits(initial['m_GravityModifier'])[0],
        limiterLimit=limits(clamp['m_Magnitude'])[0], limiterDampen=clamp['m_Dampen'])
    for field, key in [('Life','m_StartLifetime'),('Speed','m_StartSpeed'),('Size','m_StartSize'),('Rotation','m_StartRotation')]:
        low, high = limits(initial[key]); values['limiter'+field+'Min']=low; values['limiter'+field+'Max']=high
    values['limiterForceMin'], values['limiterForceMax'] = limits(emitter['m_ForceModule']['m_Y'])
    code = '#pragma once\nnamespace hk3ds::huddata {\n'
    code += ''.join(f'inline constexpr float {k} = {v:.9f}f;\n' for k,v in values.items())
    code += f'inline constexpr unsigned limiterCapacity = {initial["m_MaxNumParticles"]};\n'
    code += 'inline constexpr float limiterSize[] = {'+','.join(f'{limits(emitter["m_SizeModule"]["m_Curve"],i/31)[0]:.9f}f' for i in range(32))+'};\n'
    gradient = emitter['m_ColorModule']['m_Gradient']['m_MaxGradient']
    alpha = [(gradient[f'm_Atime{i}']/65535,gradient[f'm_Key{i}']['m_A']) for i in range(gradient['m_NumAlphaKeys'])]
    def sample(t):
        for (a,x),(b,y) in zip(alpha,alpha[1:]):
            if t <= b: return x+(y-x)*(t-a)/(b-a)
        return alpha[-1][1]
    code += 'inline constexpr float limiterAlpha[] = {'+','.join(f'{sample(i/31):.9f}f' for i in range(32))+'};\n}\n'
    header = ROOT / 'include/hud_limiter_data.hpp'
    if not header.exists() or header.read_text() != code: header.write_text(code, encoding='utf-8', newline='\n')
    return dict(source_sha256=mapping['source_sha256'], clocks=values, capacity=initial['m_MaxNumParticles'],
                fragments=round(values['limiterRate']*values['limiterEmissionSeconds']),
                approximation='Deterministic cone sampling and 60 Hz stepping with the shared measured 3D velocity clamp; Unity global RNG is not reproduced')


def geo_font_source(root):
    offline = root / '_private/research/hud-geo-font'
    capture = root / '_private/research/hud-liquid-capture'
    if not (offline / 'font-map.json').is_file() and (capture / 'font-map.json').is_file():
        return capture
    return offline


def geo_font(source):
    mapping = json.loads((source / 'font-map.json').read_text())
    for filename, expected in mapping['source_sha256'].items():
        if hashlib.sha256((source / filename).read_bytes()).hexdigest() != expected:
            raise ValueError('Native Geo font hash mismatch: ' + filename)
    atlas = Image.open(source / mapping['atlas']).convert('RGBA')
    scale = mapping['transform_scale'] * mapping['pixel_scale'] * mapping['character_size'] * 16 * 1.15
    glyphs = {}
    with (source / mapping['glyphs']).open() as stream:
        rows = {chr(int(row['code'])): {k:float(v) for k,v in row.items()} for row in csv.DictReader(stream)}
    baseline = rows['0']['maxY'] * scale
    for char, row in rows.items():
        width, height = int(row['maxX']-row['minX']), int(row['maxY']-row['minY'])
        if width and height:
            tl = row['tlX']*atlas.width, (1-row['tlY'])*atlas.height
            tr = row['trX']*atlas.width, (1-row['trY'])*atlas.height
            bl = row['blX']*atlas.width, (1-row['blY'])*atlas.height
            alpha = atlas.getchannel('A').transform((width,height),Image.Transform.AFFINE,
                ((tr[0]-tl[0])/width,(bl[0]-tl[0])/height,tl[0],
                 (tr[1]-tl[1])/width,(bl[1]-tl[1])/height,tl[1]),Image.Resampling.NEAREST)
            # Rasterize the original atlas at the final LCD density; do not
            # enlarge a glyph already reduced for the former smaller HUD.
            alpha = alpha.resize((max(1,round(width*scale*VITALS_SCALE)),
                                  max(1,round(height*scale*VITALS_SCALE))),Image.Resampling.BOX)
            bitmap = Image.new('RGBA',alpha.size,'white');bitmap.putalpha(alpha)
            left, top = row['minX']*scale/16, (row['maxY']*scale-baseline)/16
            geometry = left,(row['minY']*scale-baseline)/16,row['maxX']*scale/16,top
        else:
            bitmap, geometry = None, None
        glyphs[char] = bitmap, geometry, row['advance']*scale
    return glyphs, mapping


def convert(source, output, romfs, tex3ds, font_source=None):
    for path in (output, romfs):
        if not path.absolute().is_relative_to(ROOT / '_private'):
            raise ValueError('Original resources must remain private')
        path.mkdir(parents=True, exist_ok=True)
    mapping = json.loads((source / 'hud-map.json').read_text())
    for filename, expected in mapping['source_sha256'].items():
        if hashlib.sha256((source / filename).read_bytes()).hexdigest() != expected:
            raise ValueError('Source hash mismatch: ' + filename)
    liquid_source = ROOT / '_private/research/hud-liquid'
    liquid_mapping = json.loads((liquid_source / 'liquid-map.json').read_text())
    for filename, expected in liquid_mapping['source_sha256'].items():
        if hashlib.sha256((liquid_source/filename).read_bytes()).hexdigest()!=expected:
            raise ValueError('Liquid source hash mismatch: '+filename)
    layout_source = ROOT / '_private/research/hud-layout'
    layout_mapping = json.loads((layout_source/'layout-map.json').read_text())
    for filename, expected in layout_mapping['source_sha256'].items():
        if hashlib.sha256((layout_source/filename).read_bytes()).hexdigest()!=expected:
            raise ValueError('Layout source hash mismatch: '+filename)
    relative = layout_mapping['relative']
    values = {f'source{label}{axis}':relative[name][i]
              for name,label in (('mask','Mask'),('geo','Coin'),('text','Text'),('pending','Pending'))
              for i,axis in enumerate(('X','Y'))}
    values.update(sourceMaskScale=relative['mask'][2],
                  sourceMaskSpacing=relative['mask2'][0]-relative['mask'][0])
    generated = '#pragma once\nnamespace hk3ds::huddata {\n' + ''.join(
        f'inline constexpr float {key} = {value:.9f}f;\n' for key,value in values.items()) + '}\n'
    header = ROOT/'include/hud_layout_data.hpp'
    if not header.exists() or header.read_text()!=generated:header.write_text(generated,encoding='utf-8', newline='\n')
    library = json.loads((source / mapping['libraries']['20665']).read_text())['m_Structure']['clips']
    by_name = {c['name']: c for c in library}
    clips = [dict(by_name[name]) for name in CLIPS]
    collections = {}
    for key, entry in mapping['collections'].items():
        collections[int(key)] = (json.loads((source / entry['collection']).read_text())['m_Structure'],
                                 Image.open(source / entry['texture']).convert('RGBA'))
    detail_scales = {}
    for clip in clips:
        draw_scale = relative['mask'][2] if clip['name'].startswith(('Health', 'Blue')) else 1
        for frame in clip['frames']:
            key = frame['spriteCollection']['m_PathID'], frame['spriteId']
            detail_scales[key] = max(detail_scales.get(key, 0), draw_scale)
    detail_scales[(21155, 152)] = 1  # Soul eyes, at the same density as the vessel.
    sprites = {}
    def original(key):
        if key not in sprites:
            collection, texture = collections[key[0]]
            bitmap, geometry = decode_sprite(texture, collection['spriteDefinitions'][key[1]])
            draw_scale = detail_scales.get(key)
            if key[0] == 21919:
                draw_scale = liquid_mapping['liquid_scale']
            sprites[key] = (reduced(bitmap, geometry, draw_scale) if draw_scale is not None
                            else reduced(bitmap)), geometry
        return key
    for clip in clips:
        clip['keys'] = [original((frame['spriteCollection']['m_PathID'], frame['spriteId']))
                        for frame in clip['frames']]
    def static(name, key, bitmap, geometry):
        if name.startswith(('Digit', 'Glyph ', 'LCD Glyph ', 'Prompt Glyph ', 'Lore Glyph ', 'Credit ', 'Area Title ')):
            bitmap_font = name.startswith(('Digit', 'Glyph '))
            density = (VITALS_SCALE if bitmap_font else
                       LAYOUT['panelScale'] if name.startswith(('LCD Glyph ', 'Lore Glyph ')) else 1)
            # Geo's captured bitmap metrics use the baked pixel grid. SDF text
            # keeps fractional bearings until final positioned-glyph snapping.
            bitmap, geometry = lcd_text(bitmap, geometry, raster_scale=density,
                                        snap_bearings=bitmap_font)
        sprites[key] = bitmap, geometry
        clips.append(dict(name=name, fps=30, wrapMode=6, loopStart=0, keys=[key]))
    for name, sid in [('Soul Eyes', 152)]:
        key = original((21155, sid))
        clips.append(dict(name=name, fps=30, wrapMode=6, loopStart=0, keys=[key]))
    feedback, feedback_code = append_feedback(static, clips, sprites, original, by_name)
    limiter = append_limiter(static, clips, sprites, original, by_name)
    # The fixed silhouette modulates moving liquid alpha in the GPU combiner.
    # An inverse-mask black quad would erase transparent HUD art over the world.
    mask_size = liquid_mapping['mask_size']
    shape = Image.open(liquid_source/liquid_mapping['alpha']).convert('RGBA')
    shape = shape.resize(tuple(math.ceil(v*16*VITALS_SCALE) for v in mask_size),Image.Resampling.BOX)
    geometry = -mask_size[0]/2,-mask_size[1]/2,mask_size[0]/2,mask_size[1]/2
    static('Soul Mask', ('soul-mask',0), shape, geometry)
    liquid_library = json.loads((source / mapping['libraries']['20843']).read_text())['m_Structure']['clips']
    for source_clip in liquid_library:
        if source_clip['name'] not in ('Idle', 'Shrink'):
            continue
        clip = dict(source_clip, name='Liquid ' + source_clip['name'])
        clip['keys'] = [original((f['spriteCollection']['m_PathID'], f['spriteId'])) for f in clip['frames']]
        clips.append(clip)
    # The original TextMesh size preserves native glyph ink, bearings and advances.
    glyphs, font_mapping = geo_font(font_source or geo_font_source(ROOT))
    if layout_mapping['text_anchor'] != 3 or layout_mapping['font_size'] != font_mapping['font_size']:
        raise ValueError('Geo glyphs require the authored MiddleLeft anchor and matching font size')
    advances = {}
    for char in '0123456789+- ':
        bitmap, geometry, advance = glyphs[char]
        name = f'Digit{char}' if char.isdigit() else 'Glyph ' + ('Plus' if char == '+' else 'Minus')
        if char != ' ':
            static(name, ('glyph', char), bitmap, geometry)
        advances[char] = advance
    generated = '#pragma once\nnamespace hk3ds::huddata {\n'
    generated += 'inline constexpr float geoAdvance[] = {' + ','.join(f'{advances[c]:.8f}f' for c in '0123456789+- ') + '};\n'
    # TextMesh MiddleLeft anchors the native line box about its transform.
    half_height = font_mapping['font_size']*font_mapping['character_size']*font_mapping['pixel_scale']*font_mapping['transform_scale']*16/2
    generated += f'inline constexpr float geoLineHalfHeight = {half_height:.9f}f;\n}}\n'
    header = ROOT / 'include/hud_glyphs.hpp'
    if not header.exists() or header.read_text() != generated:
        header.write_text(generated, encoding='utf-8', newline='\n')
    values = dict(liquidX=liquid_mapping['liquid_x'],liquidScale=liquid_mapping['liquid_scale'],
        liquidBottomY=-.59-liquid_mapping['mask_y'],liquidYPerSoul=.0171,
        sourceFrameX=liquid_mapping['frame_offset'][0],sourceFrameY=liquid_mapping['frame_offset'][1],
        eyesOffsetX=liquid_mapping['eyes_offset'][0],eyesOffsetY=liquid_mapping['eyes_offset'][1])
    generated = '#pragma once\nnamespace hk3ds::huddata {\n' + ''.join(
        f'inline constexpr float {key} = {value:.9f}f;\n' for key,value in values.items()) + feedback_code + '}\n'
    header = ROOT/'include/hud_liquid_data.hpp'
    if not header.exists() or header.read_text()!=generated:header.write_text(generated,encoding='utf-8', newline='\n')
    credits = convert_credits()
    append_credits(static, credits)
    lore_characters = append_lore(static, romfs)
    prompts = append_prompts(ROOT / '_private/research/tutorial-reminders', static, set(lore_characters).union(''.join(t['text'] for t in credits['texts'])))
    area_title = append_area_title(static)
    # Strip alpha-only margins and preserve the original pivot in geometry.
    for key, (bitmap, geometry) in sprites.items():
        box = bitmap.getbbox()
        if box is None:
            # Empty Soul fill still needs valid source bounds for AnimationBank.
            sprites[key] = Image.new('RGBA', (1, 1)), geometry
            continue
        left, bottom, right, top = geometry
        sx, sy = (right - left) / bitmap.width, (top - bottom) / bitmap.height
        sprites[key] = bitmap.crop(box), (left + box[0]*sx, top - box[3]*sy,
                                         left + box[2]*sx, top - box[1]*sy)
    # Share byte-identical bitmaps while retaining distinct source frame/pivot
    # records. In particular, repeated mask frames and rounded Soul levels.
    aliases, unique = {}, {}
    for key, (bitmap, geometry) in sprites.items():
        identity = (bitmap.size, bitmap.tobytes())
        if identity in unique:
            aliases[key] = unique[identity]
        else:
            unique[identity] = key
    # Share the lossless packer with the other explicitly budgeted atlases.
    from atlas_packing import pack_pages
    unique_keys = [key for key in sprites if key not in aliases]
    # Balance the same 768 KiB allocation between RGBA8 and LA8 without resizing.
    packed, pages, formats = mixed_pages([(i, *sprites[key]) for i, key in enumerate(unique_keys)],
                                         [COLOR_PAGE_SIZE], [TEXT_PAGE_SIZE], balance=True)
    allocation = texture_bytes(pages, formats)
    if allocation > 768*1024:
        raise ValueError('HUD art and text exceed the 768 KiB texture budget')
    records = {key: packed[i] for i, key in enumerate(unique_keys)}
    for key, original in aliases.items():
        records[key] = (*records[original][:5], *sprites[key][1])
    keys = list(records)
    indices = {key: i for i, key in enumerate(keys)}
    binary = output / 'hud-visual.bin'
    with binary.open('wb') as stream:
        stream.write(struct.pack('<4sIIII', b'HKA1', len(pages), len(keys), len(clips), sum(len(c['keys']) for c in clips)))
        for key in keys:
            stream.write(struct.pack('<5I4f', *records[key]))
        first = 0
        for clip in clips:
            stream.write(struct.pack('<48sfIIII', clip['name'].encode(), clip['fps'], clip['wrapMode'],
                                     clip['loopStart'], first, len(clip['keys'])))
            first += len(clip['keys'])
        for clip in clips:
            for key in clip['keys']:
                stream.write(struct.pack('<I', indices[key]))
    textures = []
    for index, (page, fmt) in enumerate(zip(pages, formats)):
        png, t3x = output / f'hud-page-{index}.png', output / f'hud-page-{index}.t3x'
        page.save(png)
        tex3ds_cache.run([str(tex3ds), '-f', fmt, '-z', 'none', '-o', str(t3x), str(png)], check=True)
        textures.append(t3x)
    for path in (binary, *textures):
        shutil.copyfile(path, romfs / path.name)
    generated = Path(__file__).with_name('hud_data.hpp.in').read_text()
    generated = generated.replace('@WIDTH@', str(pages[0].width)).replace('@HEIGHT@', str(pages[0].height))
    generated = generated.replace('@BYTES@', str(allocation))
    header = ROOT / 'include/hud_data.hpp'
    if not header.exists() or header.read_text() != generated:
        header.write_text(generated, encoding='utf-8', newline='\n')
    manifest = dict(format='HKA1', build_id=mapping['build_id'], pages=len(pages), page_size=list(pages[0].size),
                    page_sizes=[list(p.size) for p in pages], texture_formats=formats,
                    texture_format='mixed', texture_bytes=allocation, pixels_per_unit=16.0,
                    raster_policy=dict(minimum_source_scale=SCALE, vitals_scale=VITALS_SCALE,
                                       texels_per_lcd_pixel=1, sampler='nearest'),
                    source_sha256=mapping['source_sha256'], liquid=liquid_mapping, layout=layout_mapping,
                    glyph_advance=advances, geo_font=font_mapping, prompts=prompts, credits=credits, area_title=area_title,
                    identical_bitmap_aliases=len(aliases), feedback=feedback, limiter=limiter,
                    sprites=len(keys), clips=[{k:c[k] for k in ('name','fps','wrapMode','loopStart')} |
                                             {'frames':len(c['keys'])} for c in clips],
                    approximations=['Original shader alpha mask sampled at final HUD density',
                                    'Original native Unity Geo glyph raster sampled at final HUD density'])
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    print(f'HUD: {len(keys)} sprites, {len(clips)} clips, {allocation} texture bytes')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / '_private/research/hud')
    parser.add_argument('--output', type=Path, default=ROOT / '_private/converted/hud')
    parser.add_argument('--romfs', type=Path, default=ROOT / '_private/converted/hero/romfs')
    parser.add_argument('--tex3ds', type=Path, default=Path(TEX3DS_EXE))
    parser.add_argument('--font-source', type=Path)
    args = parser.parse_args()
    convert(args.source, args.output, args.romfs, args.tex3ds, args.font_source)
