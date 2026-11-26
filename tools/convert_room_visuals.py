"""Build bounded private HKR4 room artwork from exported SpriteRenderers.

Native texture pixels and tight mesh alpha are retained. Source depth drives
runtime perspective/parallax; general MeshRenderer animation is not reproduced.
Validated solid Scenemap TileMap cells retain their exact geometric coverage.
"""
import converted_files  # Install hardlink copy-on-write before asset writes.
import argparse
import base64
import hashlib
import json
import math
import shutil
import struct
import tex3ds_cache
from pathlib import Path
from PIL import Image, ImageChops, ImageDraw
from assetripper_client import ROOT
from room_tilemap import solid_cells, merge_cells, validate_material
from scene_borders import authored_scene_borders
from scene_lighting import SCENE_LIGHT_SHA256, scene_ambient
from room_filtering import sprite_footprints, prefilter, nearest_rgba4
from room_blur import split_depth
from room_startup import world_translations, project_with_startup
from room_border_masks import cutout_masks, border_parts
from inspect_fsm import decode_fsm


def initial_fade_hidden(fsm):
    """Recognize a cosmetic Fade FSM that hides its own sprite on entry."""
    if fsm.get('globalTransitions') or fsm.get('manualUpdate'):
        return False
    state = next((s for s in fsm['states'] if s['name'] == fsm['start']), None)
    # A static omission is safe only for a one-shot startup fade.  Any
    # transition from the entry state can reveal or re-hide the draw later;
    # retaining the artwork lets the runtime own that state instead of making
    # the initial shortcut permanent.
    if state is None or state.get('transitions'):
        return False
    actions = [a for a in state['actions'] if a['enabled']]
    if len(actions) != 1 or actions[0]['type'] != 'HutongGames.PlayMaker.Actions.iTweenFadeTo':
        return False
    p = actions[0]['parameters']
    def constant(name, value):
        field = p.get(name, {})
        return field.get('useVariable') is False and field.get('value') == value
    duration = p.get('time', {})
    seconds = duration.get('value', -1)
    return (p.get('gameObject', {}).get('ownerOption') == 0 and
            constant('alpha', 0) and constant('delay', 0) and
            constant('namedValueColor', '_Color') and
            duration.get('useVariable') is False and isinstance(seconds, (int, float)) and
            math.isfinite(seconds) and 0 <= seconds <= .010001 and
            p.get('loopType') == 0 and not p.get('startEvent') and not p.get('finishEvent'))


def starts_visible(item):
    """Serialized-inactive renderers stay out of the room unless a rule marks them.

    Room exports keep sprites of inactive owners (breakable debris needs them),
    but FSM/save-driven activation of those owners is not converted: drawing them
    showed pickup flashes, infection and lift parts in Town from the start.
    _include_inactive marks the converted cases (breakable remnants and debris,
    additive boss visuals, Dream_Nailcollection starting sprites).
    """
    return bool(item['active'] or item.get('_include_inactive'))


def hidden_at_start(components, read):
    if any(read(key).get('m_Enabled', 1) for key in components.get('DreamPlantOrb', ())):
        return True
    for key in components.get('PlayMakerFSM', ()):
        component = read(key)
        if not component.get('m_Enabled', 1):
            continue
        raw = component['m_Structure']['fsm']
        if raw.get('name') == 'Fade' and initial_fade_hidden(decode_fsm(raw)):
            # Only one-shot entry fades are omitted. FSMs with later events
            # remain in the inventory so a future runtime controller can own
            # their visibility instead of hiding the draw permanently.
            return True
    return False


def hidden_initial_material(components, read):
    """Recognize a transparent local material waiting for an external FSM event.

    Dreamer shields serialize as visible but their startup state clears alpha.
    Only the straight-line prefix ending at a non-emitting BoolTest is static;
    timed fades, external targets and branches that emit events stay untouched.
    """
    from inspect_fsm import decode_param
    for ref in components.get('PlayMakerFSM', ()):
        component = read(ref)
        if not component.get('m_Enabled', True):
            continue
        fsm = component.get('m_Structure', {}).get('fsm', {})
        state = next((s for s in fsm.get('states', ()) if s['name'] == fsm.get('startState')), None)
        if state is None:
            continue
        data = state['actionData']; names = data['actionNames']; alpha = None
        starts = data['actionStartIndex'] + [len(data['paramName'])]
        variables = fsm.get('variables', {})
        def value(spec, group):
            if isinstance(spec, dict):
                name = spec.get('variable') or (spec.get('name') if spec.get('useVariable') else None)
                if name:
                    return next((v['value'] for v in variables.get(group, ()) if v['name'] == name), None)
                return spec.get('value')
            return spec
        for i, action in enumerate(names):
            if not data['actionEnabled'][i]:
                continue
            action = action.rsplit('.', 1)[-1]
            if action in ('GetOwner', 'SetTextMeshProColor'):
                continue
            if action not in ('SetMaterialColor', 'BoolTest'):
                break
            params = {data['paramName'][j]: decode_param(data, j, fsm['dataVersion'])
                      for j in range(starts[i], starts[i + 1])}
            if action == 'BoolTest':
                test = value(params.get('boolVariable'), 'boolVariables')
                event = params.get('isTrue' if test else 'isFalse')
                if test is not None and not event and alpha == 0:
                    return True
                break
            target = params.get('gameObject', {})
            material = params.get('material', {})
            if (not isinstance(target, dict) or not isinstance(material, dict) or
                    target.get('ownerOption') != 0 or material.get('useVariable') or
                    material.get('value', {}).get('m_PathID', 0) or
                    value(params.get('materialIndex'), 'intVariables') != 0 or
                    value(params.get('namedColor'), 'stringVariables') != '_Color'):
                break
            color = value(params.get('color'), 'colorVariables')
            alpha = color.get('a') if isinstance(color, dict) else None
    return False


def mesh_renderer_omissions(mapping, tilemaps, glow=None):
    """Subtract admitted renderer identities from the saved mesh inventory.

    Some exports filter TileMap renderers out of the general mesh search, so
    subtracting admission counts can produce negative or understated omissions.
    """
    known = {row['renderer'] for row in mapping.get('mesh_renderer_audit', [])}
    known.update(f"{row['collection']}-{row['path']['D']}.json"
                 for row in mapping.get('mesh_renderers', []))
    admitted = {row['renderer'] for row in tilemaps}
    admitted.update(mapping.get('_tk2d_admitted', ()))
    if glow:
        admitted.update(renderer for group in glow['groups']
                        for renderer in group['parts']['MeshRenderer'])
    return sorted(known - admitted)


def sprite_tint(color, materials, ambient):
    """Sprites/Lit's diffuse fallback receives the room light; haze is unlit."""
    rgba=[color[f'm_{c}'] for c in 'RGBA']
    lit=[m for m in materials if m['m_Name']=='Crossroads_Material']
    if lit:
        if len(materials)!=1:
            raise ValueError('Unsupported multi-material lit room sprite')
        colors=lit[0]['m_SavedProperties']['m_Colors']
        for name in ('_Color','_RendererColor'):
            rgba=[v*colors[name][f'm_{c}'] for v,c in zip(rgba,'RGBA')]
        rgba[:3]=[v*light for v,light in zip(rgba,ambient)]
    if not all(math.isfinite(v) for v in rgba):
        raise ValueError('Non-finite room sprite tint')
    # HKR/Citro2D uses LDR tints. Preserve alpha and colour within that range;
    # source HDR channels (e.g. Crossroads_30's 1.00735 red) clip at the limit.
    return [max(0.,min(1.,v)) for v in rgba]
from grass_sway import SUPPORTED_MATERIAL_NAMES as GRASS_MATERIAL_NAMES, parameters as grass_parameters
from unity_transform import project  # Shared with scene_walls (terrain).
from tool_paths import TEX3DS as TEX3DS_EXE

ADDITIVE_OWNER_NAMESPACE = 1
ADDITIVE_OWNER_TAG = 0x80000000
ADDITIVE_OWNER_LOCAL_MASK = 0x00ffffff


def additive_owner_id(owner, namespace=ADDITIVE_OWNER_NAMESPACE):
    """Give an additive-scene owner a stable ID disjoint from primary scene IDs."""
    if not 1 <= namespace <= 0x7f or not 0 <= owner <= ADDITIVE_OWNER_LOCAL_MASK:
        raise ValueError('Additive visual owner is outside the namespaced ID range')
    return ADDITIVE_OWNER_TAG | (namespace << 24) | owner


def draw_owner_id(item):
    owner = int(item['go'].rsplit('-', 1)[1].split('.')[0])
    namespace = item.get('_owner_namespace')
    return additive_owner_id(owner, namespace) if namespace is not None else owner




def decode(atlas, sprite):
    if '_tk2d_definition' in sprite:
        from room_tk2d_artwork import decode as decode_tk2d
        return decode_tk2d(atlas, sprite)
    rd = sprite['m_RD']; rect = rd['m_TextureRect']; ppu = sprite['m_PixelsToUnits']
    rotation = (rd['m_SettingsRaw'] >> 2) & 15
    if rotation not in (0,1,2,3):
        raise ValueError('Unsupported rotated texture packing')
    x,y = round(rect['m_X']),round(rect['m_Y'])
    w,h = round(rect['m_Width']),round(rect['m_Height'])
    image = atlas.crop((x,atlas.height-y-h,x+w,atlas.height-y))
    if rotation in (1,3): image = image.transpose(Image.Transpose.FLIP_LEFT_RIGHT)
    if rotation in (2,3): image = image.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
    pivot, full, offset = sprite['m_Pivot'],sprite['m_Rect'],rd['m_TextureRectOffset']
    left = (offset['m_X']-pivot['m_X']*full['m_Width']) / ppu
    bottom = (offset['m_Y']-pivot['m_Y']*full['m_Height']) / ppu
    # Rasterize serialized tight triangles into alpha so neighboring packed
    # sprite texels cannot leak through rectangular C2D quads.
    vd = rd['m_VertexData']; raw=base64.b64decode(vd['m_Data'])
    channel=vd['m_Channels'][0]
    if channel['m_Stream'] != 0 or channel['m_Format'] != 0 or channel['m_Dimension'] != 3:
        raise ValueError('Unsupported Sprite vertex layout')
    count=vd['m_VertexCount']; stride=len(raw)//count
    # Stream trailing padding means stride must be inferred from all channels.
    stride=max(c['m_Offset']+4*c['m_Dimension'] for c in vd['m_Channels'] if c['m_Stream']==0)
    points=[struct.unpack_from('<3f',raw,i*stride+channel['m_Offset']) for i in range(count)]
    indices=struct.unpack('<'+'H'*(len(base64.b64decode(rd['m_IndexBuffer']))//2),base64.b64decode(rd['m_IndexBuffer']))
    mask=Image.new('L',image.size);draw=ImageDraw.Draw(mask)
    for i in range(0,len(indices),3):
        draw.polygon([((points[j][0]-left)*ppu,h-(points[j][1]-bottom)*ppu) for j in indices[i:i+3]],fill=255)
    image.putalpha(ImageChops.multiply(image.getchannel('A'),mask))
    return image,(left,bottom,left+w/ppu,bottom+h/ppu)


def encode_pages(jobs, tex3ds):
    """Retain the source PNG; round RGBA4 inputs before tex3ds truncation."""
    for t3x, (png, target, texture_format) in jobs.items():
        encoded_png = png
        if texture_format == 'rgba4':
            encoded_png = png.with_name(png.stem+'-rgba4.png')
            with Image.open(png) as image:
                nearest_rgba4(image.convert('RGBA')).save(encoded_png)
        tex3ds_cache.run([str(tex3ds),'-f',texture_format,'-z','none','-o',str(t3x),str(encoded_png)],check=True)
        shutil.copyfile(t3x,target)


def merge_additive_visuals(mapping, source, additive_sources):
    """Merge selected owner subtrees from sibling scene visual exports."""
    audit=[]
    for namespace,source_spec in enumerate(additive_sources or (),1):
        folder,root_name,*inactive_names=source_spec
        inactive_names=set(inactive_names[0]) if inactive_names else set()
        folder=Path(folder)
        relative=folder.resolve().relative_to(Path(source).resolve()).as_posix().rstrip('/')+'/'
        extra=json.loads((folder/'room-visuals-map.json').read_text())
        if extra.get('scene') not in ('Crossroads_10','False_Knight','Fungus2_15_boss'):
            raise ValueError('Unexpected additive visual scene: '+str(extra.get('scene')))
        def remap(value):
            if isinstance(value,str) and value.endswith(('.json','.png')):return relative+value
            if isinstance(value,list):return [remap(item) for item in value]
            if isinstance(value,dict):return {key:remap(item) for key,item in value.items()}
            return value
        selected=[]
        for item in extra['inventory']:
            names=[]
            for owner in item['owners']:
                go=json.loads((folder/owner).read_text())
                names.append(go['m_Name'])
            if root_name in names and (item['active'] or
                                       (names[0] in inactive_names and item.get('sprite'))):
                merged=remap(item)
                merged['_owner_namespace']=namespace
                if not item['active']:merged['_include_inactive']=True
                selected.append(merged)
        if not selected:
            raise ValueError(f'No additive SpriteRenderers under {root_name}')
        mapping['inventory'].extend(selected)
        mapping['sprites'].update({relative+key:relative+value for key,value in extra['sprites'].items()
                                   if any(item['sprite']==key for item in extra['inventory']
                                          if root_name in [json.loads((folder/owner).read_text())['m_Name']
                                                           for owner in item['owners']])})
        for name,sha in extra['source_sha256'].items():
            mapped=relative+name
            if mapped in mapping['source_sha256'] and mapping['source_sha256'][mapped]!=sha:
                raise ValueError('Conflicting additive source hash: '+mapped)
            mapping['source_sha256'][mapped]=sha
        audit.append(dict(scene=extra['scene'],owner=root_name,renderers=len(selected),
                          sprites=sorted({item['sprite'] for item in extra['inventory']
                                          if item['sprite'] and any(root_name==json.loads((folder/owner).read_text())['m_Name']
                                                                    for owner in item['owners'])})))
    return audit


def exclude_native_gate_owners(gate_profile, native_draws):
    owners = {draw['owner'] for draw in native_draws}
    return [gate for gate in gate_profile if gate['gate'] not in owners], owners


def convert(source,output,romfs,tex3ds,max_pages=12,texture_format='rgba8',plant_source=None,compact_tiles=False,grass_research=None,dynamics_source=None,tablet_glow_source=None,quality_profile=None,texture_scale=1,texture_jobs=None,compact_tile_limit=254,preserve_source_dimensions=False,additive_sources=None):
    if preserve_source_dimensions and texture_scale!=1:
        raise ValueError('Source texture dimensions require texture_scale=1')
    for p in (output,romfs):
        if not converted_files.private_output(p,ROOT,allow_build=True):raise ValueError('Output must remain private')
        p.mkdir(parents=True,exist_ok=True)
    mapping=json.loads((source/'room-visuals-map.json').read_text())
    from room_tk2d_artwork import merge as merge_tk2d
    tk2d_inventory=merge_tk2d(mapping,source)
    additive_audit=merge_additive_visuals(mapping,source,additive_sources)
    ambient=mapping.get('scene_ambient_rgb') or scene_ambient()
    verified_json={}
    for name,sha in mapping['source_sha256'].items():
        raw=(source/name).read_bytes()
        if hashlib.sha256(raw).hexdigest()!=sha:raise ValueError('Changed source: '+name)
        if name.endswith('.json'):verified_json[name]=raw
    cache={}
    def read(name):
        if name not in cache:
            raw=verified_json.pop(name) if name in verified_json else (source/name).read_bytes()
            cache[name]=json.loads(raw)
        return cache[name]
    if mapping['scene'] == 'Dream_Nailcollection':
        starting = {'level394-3372.json': 'sharedassets394.assets-236.json',
                    'level394-3228.json': 'sharedassets394.assets-320.json',
                    'level394-3279.json': 'sharedassets394.assets-350.json'}
        for item in mapping['inventory']:
            if item['renderer'] in starting:
                item['sprite'] = starting[item['renderer']]
                item['_include_inactive'] = True
    if mapping['scene'] != 'Tutorial_01':
        remnant_roots=set(mapping.get('breakable_debris_roots', []))
        for item in mapping['inventory']:
            components=item.get('components',{})
            for key in components.get('Breakable',[])+components.get('BreakableObject',[]):
                prefix=key.rsplit('-',1)[0]
                data=read(key)['m_Structure']
                for ref in data.get('remnantParts',[])+data.get('debrisParts',[])+data.get('flingDebris',[]):
                    if ref.get('m_PathID') and not ref.get('m_FileID'):
                        remnant_roots.add(f"{prefix}-{ref['m_PathID']}.json")
        for item in mapping['inventory']:
            if remnant_roots.intersection(item.get('owners',[])):
                item['_include_inactive']=True
    from candle_glow_artwork import prepare as prepare_candles, animate_chain, compile_bank as compile_candles
    candle_profile=prepare_candles(source,mapping,read)
    startup_offsets=world_translations(mapping,read)
    transform_offsets={id(read(key)):value for key,value in startup_offsets.items()}
    def project(point,chain,source_chain=None):
        return project_with_startup(point,chain,transform_offsets,source_chain)
    quality_cuts=[]
    if quality_profile is not None:
        from room_quality import PROFILE, select_cuts
        if quality_profile!=PROFILE:raise ValueError('Unknown artwork quality profile')
        quality_cuts=select_cuts(mapping,read)
    cut_owners={row['owner'] for row in quality_cuts}
    artwork={key:(source/texture,read(key)) for key,texture in mapping['sprites'].items()}
    from scene_levers import gate_artwork, bind_gate_artwork
    gate_profile,gate_sprites=gate_artwork(mapping,source,read)
    artwork.update(gate_sprites)
    from scene_shrooms import artwork as shroom_artwork, bind_artwork as bind_shroom_artwork
    shroom_profile,shroom_sprites=shroom_artwork(mapping,source,read)
    artwork.update(shroom_sprites)
    grass_inventory=grass_push=grass_sources=grass_particle_origins=None
    if grass_research is not None:
        from grass_artwork import load as load_grass
        grass_inventory,grass_push,grass_sources,grass_particle_origins=load_grass(grass_research)
        for path_id,sprite in grass_sources.items():
            key=f'resources.assets-{path_id}.json'
            if key not in artwork:
                texture=sprite['m_RD']['texture']
                atlas=source/f"resources.assets-{texture['m_PathID']}.png"
                if atlas.name not in mapping['source_sha256'] or not atlas.is_file():
                    raise ValueError('Missing verified grass SpriteAtlas texture: '+atlas.name)
                artwork[key]=(atlas,sprite)
    plant_profile=None
    if plant_source is not None:
        from plant_artwork import sources
        plant_profile,extra=sources(plant_source,source,mapping)
        artwork.update(extra)
    dynamics_profile=None
    dynamics_whole=set()
    if dynamics_source is not None:
        from room_dynamics_artwork import sources as dynamics_sources
        dynamics_profile,extra,dynamics_whole=dynamics_sources(dynamics_source,source,mapping)
        artwork.update(extra)
    whole_sprites=set(dynamics_whole) | {row['sprite'] for row in tk2d_inventory}
    whole_sprites.update(gate_sprites)
    whole_sprites.update(shroom_sprites)
    if plant_profile is not None:
        from plant_artwork import sprite_key
        whole_sprites.update(sprite_key(name,key) for name,clip in plant_profile['clips'].items() for key in clip['keys'])
    # Animation frames use their serialized owner's depth/scale and their own
    # PPU. Keeping a frame whole for animation bindings must not bypass coverage.
    owner_names={int(item['go'].rsplit('-',1)[1][:-5]):item['go'] for item in mapping['inventory']}
    animation_sprites={}
    def add_frames(owner,keys):
        animation_sprites.setdefault(owner_names[owner],set()).update(keys)
    for gate in gate_profile:
        add_frames(gate['gate'],gate['frames'])
    for shroom in shroom_profile:
        add_frames(shroom['owner'],(key for clip in shroom['clips'] for key in clip['frames']))
    if plant_profile is not None:
        for plant in plant_profile['plants']+plant_profile['passive_plants']:
            clip=plant['clip']
            add_frames(plant['owner'],(sprite_key(clip,key) for key in plant_profile['clips'][clip]['keys']))
    if dynamics_profile is not None:
        for spike in dynamics_profile['spikes']:
            add_frames(spike['owner'],(key['source'] for key in dynamics_profile['keys']))
    footprints=sprite_footprints(mapping,lambda name: artwork[name][1] if name in artwork else read(name),
                                 project,animation_sprites)
    textures={}; tiles=[]; by_sprite={}; omitted=[]
    grass_sprite_keys={f'resources.assets-{i}.json' for i in (grass_sources or {})
                       if f'resources.assets-{i}.json' not in mapping['sprites']}
    for key,(texture,sprite) in artwork.items():
        if texture not in textures:textures[texture]=Image.open(texture).convert('RGBA')
        try:
            if key in grass_sprite_keys:
                from grass_artwork import decode as decode_grass
                image,geometry=decode_grass(sprite,textures[texture])
            else:
                image,geometry=decode(textures[texture],sprite)
                image=prefilter(image,footprints.get(key,0))
        except ValueError as error:
            omitted.append(dict(sprite=key,reason=str(error)));continue
        if quality_profile is not None:
            from room_quality import SOFT_SPRITES, HALF_SIZE_SPRITES, trim_fringe, resize_background
            if key in SOFT_SPRITES:image=trim_fringe(image)
            if key in HALF_SIZE_SPRITES and not preserve_source_dimensions:image=resize_background(image)
        left,bottom,right,top=geometry;by_sprite[key]=[]
        if compact_tiles and key not in whole_sprites:
            from room_tiling import compact_parts
            for part in compact_parts(image,geometry,compact_tile_limit):
                index=len(tiles);by_sprite[key].append(index)
                tiles.append(dict(part,source=key,index=index))
            continue
        for y in range(0,image.height,512):
            for x in range(0,image.width,512):
                bitmap=image.crop((x,y,min(x+512,image.width),min(y+512,image.height)))
                if not bitmap.getchannel('A').getbbox():continue
                index=len(tiles);by_sprite[key].append(index)
                bounds=(left+x*(right-left)/image.width,top-(y+bitmap.height)*(top-bottom)/image.height,
                        left+(x+bitmap.width)*(right-left)/image.width,top-y*(top-bottom)/image.height)
                tiles.append(dict(bitmap=bitmap,geometry=bounds,source=key,index=index,
                                  source_rect=(x,y,x+bitmap.width,y+bitmap.height)))
    from room_breakable_effects import compile_effects
    breakable_binary, breakable_report = compile_effects(mapping, source, read, tiles, ambient)
    (output/'room-breakable-fx.bin').write_bytes(breakable_binary)
    shutil.copyfile(output/'room-breakable-fx.bin',romfs/'room-breakable-fx.bin')
    (output/'room-breakable-fx.json').write_text(json.dumps(breakable_report,indent=2)+'\n')
    from mantis_room_art import add_tiles as add_mantis_tiles
    mantis_draws,mantis_audit=add_mantis_tiles(source,mapping,tiles)
    # The arena's native Mantis timeline owns these same gate and lever draws.
    gate_profile, mantis_owners = exclude_native_gate_owners(gate_profile, mantis_draws)
    from scene_particles import particle_source, load as load_particles, compile_config
    particles=particle_source(source)
    particle_index=len(tiles)
    if particles is not None:
        _,_,particle_images=load_particles(particles)
        for texture,image in particle_images.items():
            tiles.append(dict(bitmap=image,geometry=(-.5,-.5,.5,.5),
                              source='@scene-particles:'+texture,index=len(tiles)))
    particle_binary=(compile_config(particles,particle_index,ambient) if particles is not None
                     else struct.pack('<4s5I',b'HAP1',2,0,0,0,0))
    (output/'room-particles.bin').write_bytes(particle_binary)
    shutil.copyfile(output/'room-particles.bin',romfs/'room-particles.bin')
    if mapping.get('scene') == 'Cutscene_Boss_Door':
        from convert_monomon_mask import add_tiles as mask_tiles
        mask_tiles(tiles, by_sprite)
    glow=None
    if tablet_glow_source is not None:
        from tablet_glow_artwork import load, add_tiles
        glow=load(tablet_glow_source)
        add_tiles(tablet_glow_source,glow,tiles,by_sprite)
    tilemap_draws=[]; tilemap_audit=[]
    from grove_grub_artwork import add_tile as grub_tile
    grub_art = grub_tile(source, mapping, tiles, by_sprite)
    if not mapping.get('tilemap') and mapping['scene']=='Tutorial_01': raise ValueError('Re-export Scenemap TileMap dependencies')
    solid_index=len(tiles)
    tiles.append(dict(bitmap=Image.new('RGBA',(4,4),(0,0,0,255)),geometry=(0,0,1,1),
                      source='@tilemap-solid',index=solid_index,solid=True))
    for item in mapping['tilemap']:
        if not item['active']: continue
        validate_material(read(item['material']),read(item['shader']))
        cells=solid_cells(read(item['mesh']),Image.open(source/item['image']).convert('RGBA'))
        rectangles=merge_cells(cells);r=read(item['renderer']);chain=[read(k) for k in item['chain']]
        owner=int(item['go'].rsplit('-',1)[1].split('.')[0])
        for left,bottom,right,top in rectangles:
            origin=project((left,bottom,0),chain);px=project((right,bottom,0),chain);py=project((left,top,0),chain)
            a,c,zx=[px[i]-origin[i] for i in range(3)];b,d,zy=[py[i]-origin[i] for i in range(3)]
            tilemap_draws.append(dict(sprite=solid_index,owner=owner,
                values=[a,b,origin[0],c,d,origin[1],origin[2],1,1,1,1,r['m_SortingLayer'],r['m_SortingOrder'],zx,zy,0],
                sort=(r['m_SortingLayer'],r['m_SortingOrder'],-origin[2],owner,solid_index)))
        tilemap_audit.append(dict(renderer=item['renderer'],mesh=item['mesh'],cells=len(cells),rectangles=len(rectangles)))
    if mapping.get('runtime_borders'):
        borders=read(mapping['runtime_borders'])['borders']
    elif mapping.get('scene')=='Tutorial_01':
        # King's Pass visuals are not a room export: its SceneManager, border
        # prefab and TileMap come from tools/psp/export_tutorial_borders.py.
        # The SceneManager formula matches the original runtime capture.
        tutorial=source.parent/'tutorial-scene/Tutorial_01'
        if not (tutorial/'borders/scene-borders-map.json').is_file():
            raise ValueError('Import runtime SceneBorders with import_room_borders.py '
                             'or export them with tools/psp/export_tutorial_borders.py')
        borders=authored_scene_borders(tutorial,'Tutorial_01')
    else:
        borders=authored_scene_borders(source.parent,mapping['scene'])
    border_masks=cutout_masks(mapping,read,tiles,by_sprite,project) if borders else []
    border_mask_texels=0
    for i,border in enumerate(borders):
        left,bottom,z,right,top,_=border['bounds']
        # Reserved IDs cannot collide with serialized Unity PathIDs.
        owner=0xfffffff0+i;layer=border['layer'];order=border['order']
        for (left,bottom,right,top),bitmap in border_parts(border['bounds'],border_masks):
            index=solid_index
            if bitmap is not None:
                border_mask_texels+=bitmap.width*bitmap.height
                if border_mask_texels>256*1024:
                    raise ValueError('Static SceneBorder mask exceeds New 3DS texel budget')
                index=len(tiles)
                tiles.append(dict(bitmap=bitmap,geometry=(0,0,1,1),
                                  source='@scene-border-mask',index=index))
            tilemap_draws.append(dict(sprite=index,owner=owner,
                values=[right-left,0,left,0,top-bottom,bottom,z,1,1,1,1,layer,order,0,0,0],
                sort=(layer,order,-z,owner,index)))
    # Deterministic shelf packing. Solid tiles have an opaque filtering border.
    if texture_scale!=1:
        if not 0<texture_scale<1:raise ValueError('Invalid scene texture scale')
        for tile in tiles:
            if not tile.get('solid'):
                bitmap=tile['bitmap'];tile['bitmap']=bitmap.resize((max(1,round(bitmap.width*texture_scale)),max(1,round(bitmap.height*texture_scale))),Image.Resampling.LANCZOS)
    pages=[];placements={};x=y=row=0
    for tile in sorted(tiles,key=lambda t:(-t['bitmap'].height,-t['bitmap'].width,t['source'],t['index'])):
        bitmap=tile['bitmap'];w,h=bitmap.size
        if not pages:pages.append(Image.new('RGBA',(1024,1024)))
        if x+w+2>1024:x=0;y+=row;row=0
        if y+h+2>1024:
            pages.append(Image.new('RGBA',(1024,1024)));x=y=row=0
        if len(pages)>max_pages:raise ValueError('Scene exceeds configured texture page budget')
        if tile.get('solid'): pages[-1].paste((0,0,0,255),(x,y,x+w+2,y+h+2))
        if 'border' in tile: pages[-1].paste(tile['border'],(x,y))
        pages[-1].paste(bitmap,(x+1,y+1));placements[tile['index']]=(len(pages)-1,x+1,y+1,w,h,*tile['geometry'])
        x+=w+2;row=max(row,h+2)
    draws=tilemap_draws+mantis_draws; grass_owners=set()
    for item in mapping['inventory']:
        if not starts_visible(item) or item['sprite'] not in by_sprite:continue
        if hidden_at_start(item['components'], read) or hidden_initial_material(item['components'], read):continue
        if mapping['scene']!='Tutorial_01' and any(k=='HealthManager' or k.startswith('NPC') or k in ('DialogueBox','ShopMenuStock') for k in item['components']):
            continue
        owner=draw_owner_id(item)
        if owner in cut_owners or owner in mantis_owners:continue
        material_objects=[read(k) for k in item['materials']]
        material_names=[m['m_Name'] for m in material_objects]
        # Keep every active SpriteRenderer whose shader has a known visual
        # contract.  The old converter silently dropped these objects, which
        # removed room beams, light blobs and darkness cutouts from Tutorial_01.
        unknown=[name for name in material_names if name not in GRASS_MATERIAL_NAMES and name not in
                 ('Crossroads_Material','Sprites-Default','SwayingGrass_Crossroads',
                 'UIBlendScreen','UIBlendLinearLight','UIBlendLinearDodge','Sprites-Darkness-Cutout')]
        if unknown and mapping['scene']=='Tutorial_01':
            omitted.append(dict(renderer=item['renderer'],reason='Unsupported material',materials=material_names));continue
        material_kind=0
        if 'UIBlendScreen' in material_names: material_kind=1
        elif 'UIBlendLinearLight' in material_names: material_kind=2
        elif 'Sprites-Darkness-Cutout' in material_names: material_kind=3
        elif 'UIBlendLinearDodge' in material_names: material_kind=5
        r=read(item['renderer']);chain=[read(k) for k in item['chain']]
        source_chain=chain
        chain=animate_chain(candle_profile,owner,chain)
        origin=list(project((0,0,0),chain,source_chain));px=project((1,0,0),chain,source_chain);py=project((0,1,0),chain,source_chain)
        a,c=px[0]-origin[0],px[1]-origin[1];b,d=py[0]-origin[0],py[1]-origin[1]
        zx,zy=px[2]-origin[2],py[2]-origin[2]
        grass=None
        if any(name in GRASS_MATERIAL_NAMES for name in material_names):
            grass_materials=[m for m in material_objects if m['m_Name'] in GRASS_MATERIAL_NAMES]
            if len(grass_materials)!=1 or len(item.get('owners',[]))<2:
                raise ValueError('Grass renderer must have one source material and a parent owner')
            pz=project((0,0,1),chain,source_chain)
            source_owner=int(item['owners'][1].rsplit('-',1)[1].split('.')[0])
            namespace=item.get('_owner_namespace')
            if namespace is not None:source_owner=additive_owner_id(source_owner,namespace)
            grass=grass_parameters(grass_materials[0],source_owner,origin,
                px[1]-origin[1]+py[1]-origin[1]+pz[1]-origin[1])
            origin[2]+=grass['z_jitter']
            grass_owners.add(source_owner)
        if r['m_FlipX']:a,c,zx=-a,-c,-zx
        if r['m_FlipY']:b,d,zy=-b,-d,-zy
        rgba=sprite_tint(r['m_Color'],material_objects,ambient)
        if item.get('_tk2d_mesh'):
            mapping.setdefault('_tk2d_admitted',set()).add(item['_tk2d_mesh'])
        for index in by_sprite[item['sprite']]:
            draws.append(dict(sprite=index,owner=owner,grass=grass,values=[a,b,origin[0],c,d,origin[1],origin[2],*rgba,r['m_SortingLayer'],r['m_SortingOrder'],zx,zy,material_kind],
                              sort=(r['m_SortingLayer'],r['m_SortingOrder'],-origin[2],owner,index)))
    if mapping.get('build')==22529139 and mapping.get('scene')=='Tutorial_01' and len(grass_owners)!=41:
        raise ValueError(f'Expected 41 source grass owners, found {len(grass_owners)}')
    if glow is not None:
        from tablet_glow_artwork import add_draws
        add_draws(tablet_glow_source,glow,draws,by_sprite,project)
    if mapping.get('scene') == 'Cutscene_Boss_Door':
        from convert_monomon_mask import add_draws as mask_draws
        mask_draws(draws, by_sprite)
    from grove_grub_artwork import add_draw as grub_draw
    grub_draw(grub_art,draws,by_sprite,read,project)
    draws.sort(key=lambda d:d['sort'])
    from mantis_room_art import write_bank as write_mantis_bank
    write_mantis_bank(mantis_audit,draws,(output,romfs))
    if glow is not None:
        from tablet_glow_artwork import compile_bank
        ambient_binary,audit=compile_bank(tablet_glow_source,glow,source,mapping,by_sprite,draws,len(tiles),project)
        (output/'room-ambient.bin').write_bytes(ambient_binary)
        shutil.copyfile(output/'room-ambient.bin',romfs/'room-ambient.bin')
        (output/'tablet-glow.json').write_text(json.dumps(audit,indent=2)+'\n',encoding='utf-8')
    candle_binary=compile_candles(candle_profile,draws)
    for directory in (output,romfs):
        path=directory/'room-visual.bin.scale'
        if candle_binary is None:path.unlink(missing_ok=True)
        else:path.write_bytes(candle_binary)
    grass_records=[]
    blur_split_depth=split_depth(mapping,read,project)
    binary=bytearray(struct.pack('<4s4I',b'HKR4',len(pages),len(tiles),len(draws),0))
    for i in range(len(tiles)):binary.extend(struct.pack('<5I4f',*placements[i]))
    for draw_index,draw in enumerate(draws):
        binary.extend(struct.pack('<2I16f',draw['sprite'],draw['owner'],*draw['values']))
        if draw.get('grass') is not None:
            g=draw['grass']
            grass_records.append((draw_index,g['speed'],g['amplitude'],g['phase'],g['push'],g['height']))
    struct.pack_into('<I',binary,16,len(grass_records))
    for record in grass_records:binary.extend(struct.pack('<I5f',*record))
    binary.extend(struct.pack('<f',blur_split_depth))
    if grass_inventory is not None:
        from grass_artwork import compile_bank
        grass_binary=compile_bank(grass_inventory,grass_push,by_sprite,draws,len(tiles),len(draws),grass_particle_origins)
        (output/'grass.bin').write_bytes(grass_binary)
        shutil.copyfile(output/'grass.bin',romfs/'grass.bin')
    else:
        (output/'grass.bin').unlink(missing_ok=True)
        (romfs/'grass.bin').unlink(missing_ok=True)
    plant_binding=None
    if plant_profile is not None:
        from plant_artwork import bind
        plant_binding=bind(plant_profile,mapping,by_sprite,draws,binary,
                           (source/'room-visuals-map.json').read_bytes())
    if dynamics_profile is not None:
        from room_dynamics_artwork import bind as dynamics_bind, compile_bank
        dynamics_binding=dynamics_bind(dynamics_profile,source,mapping,by_sprite,draws,binary)
        (output/'room-dynamics.json').write_text(json.dumps(dynamics_binding,indent=2)+'\n')
        (output/'room-dynamics.bin').write_bytes(compile_bank(dynamics_binding,binary))
        shutil.copyfile(output/'room-dynamics.bin',romfs/'room-dynamics.bin')
    else:
        (output/'room-dynamics.json').unlink(missing_ok=True)
        (output/'room-dynamics.bin').unlink(missing_ok=True)
        (romfs/'room-dynamics.bin').unlink(missing_ok=True)
    if shroom_profile:
        (output/'room-shrooms.bin').write_bytes(bind_shroom_artwork(shroom_profile,by_sprite,draws))
        shutil.copyfile(output/'room-shrooms.bin',romfs/'room-shrooms.bin')
    else:
        (output/'room-shrooms.bin').unlink(missing_ok=True)
        (romfs/'room-shrooms.bin').unlink(missing_ok=True)
    if gate_profile:
        (output/'room-levers.bin').write_bytes(bind_gate_artwork(gate_profile,by_sprite,draws))
        shutil.copyfile(output/'room-levers.bin',romfs/'room-levers.bin')
    else:
        (output/'room-levers.bin').unlink(missing_ok=True)
        (romfs/'room-levers.bin').unlink(missing_ok=True)
    (output/'room-visual.bin').write_bytes(binary);shutil.copyfile(output/'room-visual.bin',romfs/'room-visual.bin')
    if plant_binding is not None:
        (output/'plant-artwork.json').write_text(json.dumps(plant_binding,indent=2)+'\n',encoding='utf-8')
    else:
        (output/'plant-artwork.json').unlink(missing_ok=True)
    for i,page in enumerate(pages):
        png=output/f'room-page-{i}.png';t3x=output/f'room-page-{i}.t3x';page.save(png)
        job=(png,romfs/t3x.name,texture_format)
        if texture_jobs is None:encode_pages({t3x:job},tex3ds)
        else:texture_jobs[t3x]=job
    omitted_meshes=mesh_renderer_omissions(mapping,tilemap_audit,glow)
    omitted_meshes=[item for item in omitted_meshes if item not in mantis_audit.get('renderer_ids',[])]
    if grub_art is not None:
        omitted_meshes.remove(grub_art['renderer'])
    report=dict(build=mapping['build'],scene=mapping['scene'],blur_split_depth=blur_split_depth,pages=len(pages),tiles=len(tiles),draws=len(draws),
                texture_scale=texture_scale,
                texture_format=texture_format,texture_bytes=len(pages)*1024*1024*(4 if texture_format=='rgba8' else 2),
                source_sha256=mapping['source_sha256'],binary_sha256=hashlib.sha256(binary).hexdigest(),
                scene_ambient_rgb=ambient,scene_light_sha256=SCENE_LIGHT_SHA256,
                coverage_filter='premultiplied Gaussian, .35 screen pixels, least minified sharp-pass instance; approximation',
                grass_sway=dict(format='HKR3 <I5f> sparse draw records',records=len(grass_records),owners=len(grass_owners),
                    sample_method='SHA-256 owner seed hk3ds-grass-sway-v1; stable approximation, not captured Unity RNG',
                    epsilon_range=[-0.01,0.01],z_jitter_range=[-0.0001,0.0001]),
                selected_owners=len({d['owner'] for d in draws}),omitted=omitted,
                serialized_renderers=len(mapping['inventory']),
                inactive_renderers=sum(not r['active'] for r in mapping['inventory']),
                active_null_sprite_renderers=[r['renderer'] for r in mapping['inventory'] if r['active'] and r['sprite'] is None],
                tilemap=tilemap_audit,runtime_borders=borders,additive_visuals=additive_audit,mantis_visuals=mantis_audit,grub_artwork=grub_art,
                border_mask_texels=border_mask_texels,
                border_mask_approximation='static coplanar layer-1 vignette alpha; no camera blur',
                startup_world_translations=startup_offsets,
                tk2d_renderers=sorted(mapping.get('_tk2d_admitted', ())),
                omitted_mesh_renderers=len(omitted_meshes),omitted_mesh_renderer_ids=omitted_meshes,
                limitations=['Serialized initial pose plus constant owner-world startup Translate; other Awake/Start motion is not baked.','Runtime uses per-object signed LinearLight and the measured blur/vibrancy passes, with PICA TEV quantization; parallax uses the source perspective.',
                             'MeshRenderers other than admitted TileMap, tk2d artwork and Glow Bug pairs remain omitted. tk2d uses its serialized initial pose.','Tight triangle mask baked at source pixel resolution; edge raster coverage can differ from GPU.',
                             'Near-plane intersections are not clipped to source triangles.'])
    report['breakable_particles']={key:breakable_report[key] for key in ('particle_bound','sprites','pixels')}
    report['breakable_particles']['bytes']=len(breakable_binary)
    report['breakable_particles']['deferred_spawns']=len(breakable_report['deferred_spawns'])
    if plant_binding is not None:
        report['plant_artwork_sha256']=hashlib.sha256((output/'plant-artwork.json').read_bytes()).hexdigest()
    if dynamics_profile is not None:
        report['room_dynamics_sha256']=hashlib.sha256((output/'room-dynamics.json').read_bytes()).hexdigest()
    if compact_tiles:
        layout=dict(schema=1,limit=compact_tile_limit,whole_sprites=sorted(whole_sprites),
                    binary_sha256=hashlib.sha256(binary).hexdigest(),
                    tiles=[dict(index=t['index'],source=t['source'],rect=t.get('source_rect'),
                                neighbour_gutter='border' in t) for t in tiles])
        (output/'room-tiles.json').write_text(json.dumps(layout,indent=2)+'\n',encoding='utf-8')
        report['tile_layout_sha256']=hashlib.sha256((output/'room-tiles.json').read_bytes()).hexdigest()
    else:
        (output/'room-tiles.json').unlink(missing_ok=True)
    if quality_profile is not None:
        from room_quality import FRINGE_ALPHA, HALF_SIZE_SPRITES
        report['quality_cuts']=dict(profile=quality_profile,dropped_owners=quality_cuts,
                                   atmosphere_fringe_alpha=FRINGE_ALPHA,
                                   half_size_sprites=[] if preserve_source_dimensions else sorted(HALF_SIZE_SPRITES))
    (output/'manifest.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    if grass_inventory is not None:
        report['grass_artwork']=dict(owners=len(grass_inventory['owners']),push_clips=len(grass_push),
            additional_sprites=sum(f'resources.assets-{i}.json' not in mapping['sprites'] for i in grass_sources),
            bank_bytes=len(grass_binary),bank_sha256=hashlib.sha256(grass_binary).hexdigest())
        (output/'manifest.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({k:report[k] for k in ('pages','tiles','draws','texture_bytes','selected_owners','omitted_mesh_renderers')}))


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source',type=Path,default=ROOT/'_private/research/room-visuals')
    p.add_argument('--output',type=Path,default=ROOT/'_private/converted/room-visuals')
    p.add_argument('--romfs',type=Path,default=ROOT/'_private/converted/hero/romfs')
    p.add_argument('--tex3ds',type=Path,default=Path(TEX3DS_EXE))
    p.add_argument('--max-pages',type=int,default=12)
    p.add_argument('--texture-format',choices=('rgba8','rgba4'),default='rgba8')
    p.add_argument('--plant-source',type=Path,help='Admit validated plant clip sprites and write private index bindings')
    p.add_argument('--grass-research',type=Path,help='Add the saved Tutorial_01 grass SpriteAtlas frames and HKG1 owner bank')
    p.add_argument('--dynamics-source',type=Path,help='Admit source Cave Spikes frames and SimpleRock motion')
    p.add_argument('--compact-tiles',action='store_true',help='Subdivide scenery at 254 pixels and trim empty regions; plant frames stay whole')
    p.add_argument('--tablet-glow-source',type=Path,help='Admit the three tablet fades and 11 paired Bug/Glow animations')
    p.add_argument('--quality-profile',choices=('new3ds-30fps',),help='Apply reviewed static atmosphere quality cuts')
    p.add_argument('--preserve-source-dimensions',action='store_true',help='Keep source pixel dimensions, including quality-profile backgrounds')
    a=p.parse_args();convert(a.source,a.output,a.romfs,a.tex3ds,a.max_pages,a.texture_format,a.plant_source,a.compact_tiles,a.grass_research,dynamics_source=a.dynamics_source,tablet_glow_source=a.tablet_glow_source,quality_profile=a.quality_profile,preserve_source_dimensions=a.preserve_source_dimensions)
