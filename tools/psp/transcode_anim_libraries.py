"""Complete route libraries from the copied original; preserve existing effect clips.

Host banks and PSP banks are both generated in this checkout's private output.
Each bank runs in a separate process to bound UnityPy's source-texture cache.
"""
import argparse
import copy
import json
import re
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tools'))
sys.path.insert(0, str(ROOT/'tools/psp'))
import animation_audit as audit
from animation_library import event_bytes, complete_aliases
import transcode_room as room

CONVERTERS = {
    'greenpath-enemies': 'convert_greenpath_enemies.py',
    'crossroads-bosses': 'convert_crossroads_bosses.py',
    'dung-defender': 'convert_dung_defender.py',
    'uumuu-body': 'convert_uumuu_body_art.py',
    'uumuu-quirrel': 'convert_uumuu_quirrel_art.py',
}
# Identical identities used by the specialized source converters.
LIBRARIES = {
    'broken-vessel/BrokenVessel': ('sharedassets343.assets', 150),
    'broken-vessel/Balloon': ('sharedassets343.assets', 148),
    'broken-vessel/Scuttler': ('sharedassets53.assets', 39),
    'mantis-lords/Lord': ('sharedassets180.assets', 84),
    'watcher-knights/Watcher': ('sharedassets115.assets', 174),
    'elder-baldur/': ('sharedassets50.assets', 109),
    'elder-baldur/Roller': ('sharedassets50.assets', 111),
    'false-knight/Knight': ('sharedassets47.assets', 9),
    'false-knight/Head': ('sharedassets47.assets', 9),
    'monomon-archive-art/Monomon': ('sharedassets178.assets', 21),
    'monomon-guardian-art/Monomon': ('sharedassets402.assets', 8),
}
OBSOLETE = {
    'crossroads-enemies-a/tiktik': ('Idle',),
    'crossroads-enemies-a/gruzzer': ('Idle',),
    'crossroads-enemies-a/aspid-hatchling': ('Idle','Death Air','Death Land'),
    'enemy-c/sentry': ('Antic',),
    'greenpath-enemies/Maskfly': ('Death',),
    'greenpath-enemies/Aluba': ('Death',),
    'crossroads-bosses/Gruzzer': ('Death',),
}
# Scene-enemy render table (tools/psp/room_stage.py SCENE_ENEMY_RENDER): exactly the clip names of the
# simulation bank HKA1 (scene-enemies/<bank>), so every simulation clip exists in the PSP table. The
# route table above keeps only original route clips; enemy-c/sentry Antic is absent there (the route
# sentry library has no original Antic) but is still in HKA1, so only the scene table keeps it.
SCENE_BANKS = {'enemy-c-anim.bin': 'enemy-c-scene', 'greenpath-art.bin': 'greenpath-scene'}
# Pooled effect actors without a placed instance: their maximum source scale. Vomit Glob Init:
# RandomFloat 1-1.75 -> SetScale.
EFFECT_SCALES = {'enemy-c/glob': 1.75,
                 # Shot Mawlek: ScaleWithVelocity scale 1.15-1.45, stretch up to 1.75.
                 'enemy-c/shot': 1.45 * 1.75}


def hka1_clips(path):
    """Clip records (name, fps, wrapMode, loopStart, count) of a simulation HKA1 bank, in clip order."""
    data = path.read_bytes()
    if data[:4] != b'HKA1':
        raise ValueError(f'{path}: bad magic {data[:4]!r}')
    _pages, sprites, clips, _frames = struct.unpack_from('<4I', data, 4)
    pos, records = 20 + 36*sprites, []
    for _ in range(clips):
        name = data[pos:pos+48].split(b'\0', 1)[0].decode('ascii')
        fps, wrap, loop, _first, count = struct.unpack_from('<fIIII', data, pos+48)
        records.append((name, fps, wrap, loop, count))
        pos += 68
    return records


def identity(actor, facing):
    if actor['id'] in LIBRARIES:
        return LIBRARIES[actor['id']]
    family, prefix = actor['family'], actor['prefix']
    if family in CONVERTERS:
        table = audit.constants(ROOT/'tools'/CONVERTERS[family])
        if family == 'greenpath-enemies':
            return next((r[3], r[4]) for r in table['ACTORS'] if r[1] == prefix)
        return tuple(table['ALIASES'][prefix][:2])
    for filename, _ in actor['source_files']:
        match = re.fullmatch(r'(.+\.assets)-(\d+)\.json', Path(filename).name)
        if match:
            return match[1], int(match[2])
        data = json.loads(Path(filename).read_text(encoding='utf-8'))
        library = data.get('library')
        if isinstance(library, str):
            match = re.fullmatch(r'(.+\.assets)-(\d+)\.json', library)
            if match:
                return match[1], int(match[2])
    special = facing/'source-libraries'/(prefix+'.json')
    if special.exists():
        name, index = json.loads(special.read_text())['library'].split(':')
        return name, int(index)
    raise ValueError(f"{actor['id']}: no source library identity")


class Original:
    def __init__(self, root):
        sys.path.insert(0, str(root/'_private/tools/boss-python'))
        import UnityPy
        from watcher_unity import AlignedGenerator
        self.source = root/'_private/original/Hollow Knight/hollow_knight_Data'
        self.env = UnityPy.Environment(path=str(self.source))
        generator = AlignedGenerator('6000.0.61f1')
        generator.load_local_dll_folder(str(self.source/'Managed'))
        self.env.typetree_generator = generator
        self.files, self.textures, self.collections, self.materials = {}, {}, {}, {}

    def load(self, name):
        if name not in self.files:
            self.env.load_file(str(self.source/name))
            self.files[name] = next(f for f in self.env.files.values() if getattr(f, 'name', '') == name)
        return self.files[name]

    def resolve(self, owner, ref):
        name = owner.name if not ref['m_FileID'] else Path(owner.externals[ref['m_FileID']-1].path).name
        return self.load(name).objects[ref['m_PathID']]

    def sprite(self, library, frame):
        from convert_hero import decode_sprite
        obj = self.resolve(library, frame['spriteCollection'])
        key = (obj.assets_file.name, obj.path_id)
        if key not in self.collections:
            self.collections[key] = obj.read_typetree()
        collection = self.collections[key]
        definitions = collection['spriteDefinitions']
        if not definitions:
            raise ValueError(f'{key}: empty original sprite collection')
        # tk2dBaseSprite.spriteId clamps to the collection bounds (build
        # 22529139, TeamCherry.TK2D). Cornifer's unused DP clips exceed them.
        sprite_id = max(0, min(frame['spriteId'], len(definitions)-1))
        definition = definitions[sprite_id]
        material = self.resolve(obj.assets_file, collection['materials'][definition['materialId']])
        material_key = (material.assets_file.name, material.path_id)
        if material_key not in self.materials:
            self.materials[material_key] = material.read_typetree()
        ref = dict(self.materials[material_key]['m_SavedProperties']['m_TexEnvs'])['_MainTex']['m_Texture']
        texture = self.resolve(material.assets_file, ref)
        texture_key = (texture.assets_file.name, texture.path_id)
        if texture_key not in self.textures:
            self.textures[texture_key] = texture.read().image.convert('RGBA')
        return decode_sprite(self.textures[texture_key], dict(definition, materialId=0))


def prefix_for(actor, names):
    prefix = actor['prefix']
    if 'bank_prefix' in actor:
        return actor['bank_prefix']
    return prefix+'/' if prefix and any(n.startswith(prefix+'/') for n in names) else ''


def base_art(root, actor, Image):
    path = Path(actor['bank'])
    table = room.AnimationTable(path.read_bytes())
    prefix = path.stem.removesuffix('-art').removesuffix('-anim')
    candidates = [path.parent, path.parent.parent]
    for parent in ('converted', 'research', 'minimum-bosses', 'minimum-world-quests'):
        candidates += [root/'_private'/parent/actor['family']]
        candidates += sorted((root/'_private'/parent).glob('*'))
    pages = next((p for p in candidates if (p/(prefix+'-page-0.png')).is_file()), None)
    if pages is None:
        raise ValueError(f'{path}: no source PNG pages for {prefix}')
    images = []
    for n in range(table.pages):
        path = pages/f'{prefix}-page-{n}.png'
        # append_effects stores False Knight's final page under this source name.
        if prefix == 'false-knight' and n == table.pages-1 and not path.exists():
            path = pages/'false-knight-fx-page.png'
        images.append(Image.open(path).convert('RGBA'))
    sprites = {}
    for i, s in enumerate(table.sprites):
        p, x, y, w, h = s[:5]
        sprites[('base', i)] = (images[p].crop((x,y,x+w,y+h)), s[5:])
    clips = {}
    for offset in range(0, len(table.clip_bytes), 68):
        name, fps, wrap, loop, first, count = struct.unpack_from('<48sf4I', table.clip_bytes, offset)
        name = name.split(b'\0')[0].decode()
        frames = [dict() for _ in range(count)]
        clips[name] = (name, dict(fps=fps, wrapMode=wrap, loopStart=loop, frames=frames),
                       [('base', i) for i in table.frames[first:first+count]])
    return clips, sprites


def write_bank(clips, sprites, pages):
    keys = list(sprites)
    indices = {k: i for i,k in enumerate(keys)}
    frames, records = [], bytearray()
    for name, source, ids in clips:
        if len(name.encode()) >= 48:
            raise ValueError(f'Clip name too long: {name}')
        records += struct.pack('<48sf4I', name.encode(), source['fps'], source['wrapMode'],
                               source.get('loopStart', 0), len(frames), len(ids))
        frames += [indices[k] for k in ids]
    return (bytes(records), frames, event_bytes(clips))


def convert_bank(args, actors, scene_clips=None):
    """Route table by default; scene_clips (HKA1 clip records) makes the scene-enemy render table."""
    Image = room.import_pil()
    source = Original(args.source_root)
    clips, sprites = base_art(args.source_root, actors[0], Image)
    base_clips = dict(clips)  # the simulation's own clips, used where the route record differs
    source_rows = []
    for actor in actors:
        asset, index = identity(actor, args.facing)
        libraries = [(asset, index)]
        if actor['id'] == 'stag/stag':
            # extract_stag combines the Stag and station-bell libraries.
            libraries.append(('sharedassets9.assets', 202))
        originals, owners = [], {}
        for library_asset, library_id in libraries:
            library_file = source.load(library_asset)
            for clip in library_file.objects[library_id].read_typetree()['clips']:
                if clip.get('name') and clip.get('frames'):
                    originals.append(clip)
                    owners[clip['name']] = library_file
        # Snapshots must describe precisely the same original library.
        if {c['name'] for c in originals} != {c['name'] for c in actor['clips']}:
            raise ValueError(f"{actor['id']}: snapshot/library clip names differ: "
                             f"source={[c['name'] for c in originals]}, snapshot={[c['name'] for c in actor['clips']]}")
        prefix = prefix_for(actor, clips)
        for obsolete in ([] if scene_clips is not None else OBSOLETE.get(actor['id'],())): clips.pop(prefix+obsolete,None)
        aliases = dict(actor['aliases'])
        available = {c['name']: c for c in originals}
        for name, original in available.items():
            library_file = owners[name]
            ids = []
            for frame in original['frames']:
                obj = source.resolve(library_file, frame['spriteCollection'])
                key = (obj.assets_file.name, obj.path_id, frame['spriteId'])
                if key not in sprites:
                    sprites[key] = source.sprite(library_file, frame)
                ids.append(key)
            clips[prefix+name] = (prefix+name, original, ids)
        aliases = {a:t for a,t in aliases.items() if t in available}
        for alias, target in complete_aliases(originals, aliases).items():
            if alias != target:
                _, original, ids = clips[prefix+target]
                clips[prefix+alias] = (prefix+alias, original, ids)
        source_rows.append(dict(actor=actor['id'], library=[f'{a}:{i}' for a,i in libraries], prefix=prefix,
                                clips=originals, aliases=aliases))
    if actors[0]['family'] == 'uumuu-quirrel':
        for name, (_, _, base_ids) in base_clips.items():
            _, original, ids = clips[name]
            if len(ids) != len(base_ids):
                raise ValueError(f'{name}: original and simulation frame counts differ')
            clips[name] = (name, original, base_ids)
    if scene_clips is not None:
        # The simulation's own clips and sprites (base_art from HKA1 and its pages), in HKA1 order.
        # Route originals are not mixed in: a sprite shared by two simulation clips must keep one
        # PSP sprite, and per-clip route/base choices split such sprites (test_scene_enemy_render_banks).
        missing = [record[0] for record in scene_clips if record[0] not in base_clips]
        if missing:
            raise ValueError(f'scene render table lacks simulation clips: {missing}')
        clips = {record[0]: base_clips[record[0]] for record in scene_clips}
    # Remove unreferenced base sprites after original-name collision repair.
    used = {key for _, _, ids in clips.values() for key in ids}
    sprites = {key:value for key,value in sprites.items() if key in used}
    # The PSP bank resamples every retained sprite to a common screen density.
    # Max source instance scale is exported independently of the facing sign.
    facing = json.loads((args.facing/'config.json').read_text())['actors']
    # Pooled effects (no placed instance) carry their own source scale and only size their sprites.
    scale = max(abs(facing[a['id']]['instance_scale']) for a in actors if a['id'] not in EFFECT_SCALES)
    family, filename = actors[0]['family'], Path(actors[0]['bank']).name
    scene_folder = SCENE_BANKS.get(filename) if scene_clips is not None else None
    if scene_clips is not None and scene_folder is None:
        raise ValueError(f'{filename}: no scene render folder')
    # Scene tables write only their PSA1 pages/table; host bank and manifest stay with the route table.
    emit_bank(args, source, family, filename, clips, sprites, scale, source_rows, scene_folder)


def emit_bank(args, source, family, filename, clips, sprites, scale, source_rows, scene_folder=None):
    Image = room.import_pil()
    ppu = room.room_focal_length(480,272)/38.1*scale
    # Sprites of pooled effects use the larger of the bank scale and the effect's own maximum.
    effect = {}
    for name, _, ids in clips.values():
        extra = next((s for prefix, s in EFFECT_SCALES.items() if name.startswith(prefix.split('/', 1)[1] + '/')), 0)
        for key in ids:
            effect[key] = max(effect.get(key, 0), extra)
    sprite_ppu = [ppu*max(1., effect.get(key, 0)/scale) for key in sprites]
    host = (ROOT/'build/psp/scene-render-host'/family/Path(filename).stem if scene_folder
            else args.out/'host'/family/Path(filename).stem)
    host.mkdir(parents=True,exist_ok=True)
    # Raw staging pages are converter input, not runtime HKA1 pages. Large
    # source effects retain all texels until PSP's one resampling operation.
    source_places, source_count = room.pack([im.size for im,_ in sprites.values()],page=2048)
    source_pages = [Image.new('RGBA',(2048,2048)) for _ in range(source_count)]
    source_records = []
    for (im,geometry),(page,x,y) in zip(sprites.values(),source_places):
        room.paste_padded(source_pages[page],im,x,y)
        source_records.append((page,x,y,im.width,im.height,*geometry))
    staging=host/'source'
    staging.mkdir(exist_ok=True)
    for i,im in enumerate(source_pages): im.save(staging/f'page-{i}.png')
    del source_pages
    # Host pages use the established half-resolution converter contract.
    host_sprites=[(room.resample(Image,im,(max(1,round(im.width*.5)),max(1,round(im.height*.5)))),g)
                  for im,g in sprites.values()]
    places,count=room.pack([im.size for im,_ in host_sprites],page=1024)
    host_pages=[Image.new('RGBA',(1024,1024)) for _ in range(count)]
    records=bytearray()
    for (im,g),(page,x,y) in zip(host_sprites,places):
        room.paste_padded(host_pages[page],im,x,y)
        records+=struct.pack('<5I4f',page,x,y,im.width,im.height,*g)
    for i,image in enumerate(host_pages): image.save(host/f'page-{i}.png')
    clip_bytes, frames, events = write_bank(list(clips.values()),sprites,count)
    raw = (b'HKA1'+struct.pack('<4I',count,len(sprites),len(clips),len(frames))+records+
           clip_bytes+struct.pack(f'<{len(frames)}I',*frames)+events)
    (host/filename).write_bytes(raw)
    room.AnimationTable(raw)  # Reject runtime-incompatible host counts/geometry.
    factors = [(min(1.,(s[7]-s[5])*p/s[3]), min(1.,(s[8]-s[6])*p/s[4])) for s, p in zip(source_records, sprite_ppu)]
    out = args.out/family/(scene_folder or Path(filename).stem)
    if scene_folder:
        # Stale pages from an earlier, larger table would be counted as pages on disk.
        out.mkdir(parents=True,exist_ok=True)
        for stale in out.glob('page-*'): stale.unlink()
    sizes, pages, stats, sprite_bytes = room.build_pages(Image,staging,'page-{n}.png',source_count,source_records,factors,out)
    psp = (b'PSA1'+struct.pack('<6I',1,512,pages,len(sprites),len(clips),len(frames))+
           sprite_bytes+clip_bytes+struct.pack(f'<{len(frames)}I',*frames)+events)
    (out/Path(filename).with_suffix('.psp.bin')).write_bytes(psp)
    if scene_folder:
        print(json.dumps(dict(bank=filename,scene_table=str(out),clips=len(clips),sprites=len(sprites),psp_pages=pages,
                              psp_table_bytes=len(psp))),flush=True)
        return
    manifest = dict(bank=filename,family=family,actors=source_rows,clips=len(clips),sprites=len(sprites),
                    frames=len(frames),event_bytes=len(events),host_bytes=len(raw),psp_table_bytes=len(psp),
                    psp_pages=pages,texture_bytes=pages*(512*512+1040),instance_scale=scale,
                    resident_bytes=len(psp)+pages*(512*512+1040),page_stats=stats,
                    source_sha256={n:room.sha256(source.source/n) for n in source.files})
    (out/'manifest.json').write_text(json.dumps(manifest,indent=1)+'\n')
    print(json.dumps({k:manifest[k] for k in ('bank','clips','psp_pages','resident_bytes')}),flush=True)


def emit_original_clips(args, source, family, filename, original_clips, extra_sprites=()):
    clips, sprites = {}, {}
    for library, clip in original_clips:
        if not clip['name'] or not clip['frames']:
            continue
        keys = []
        for frame in clip['frames']:
            obj = source.resolve(library, frame['spriteCollection'])
            key = (obj.assets_file.name, obj.path_id, frame['spriteId'])
            if key not in sprites:
                sprites[key] = source.sprite(library, frame)
            keys.append(key)
        clips[clip['name']] = (clip['name'], clip, keys)
    if not clips:
        raise ValueError(f'Empty {family} animation library')
    for name, image, geometry in extra_sprites:
        sprites[name] = (image, geometry)
        clips[name] = (name, dict(fps=1., wrapMode=1, frames=[{}]), [name])
    emit_bank(args, source, family, filename, clips, sprites, 1., [])


def convert_zombie_guard(args):
    source = Original(args.source_root)
    library = source.load('sharedassets58.assets')
    spurt_library = source.load('sharedassets32.assets')
    guard_clips = [(library, clip) for clip in library.objects[57].read_typetree()['clips']]
    spurt = spurt_library.objects[745].read_typetree()['clips'][3]
    if spurt['name'] != 'Shockwave Spurt' or len(spurt['frames']) != 6:
        raise ValueError('Unexpected Zombie Guard stomp art')
    emit_original_clips(args, source, 'zombie-guard', 'zombie-guard-anim.bin',
                        [*guard_clips, (spurt_library, spurt)])


def convert_spitter(args):
    source = Original(args.source_root)
    actor_library = source.load('sharedassets39.assets')
    shot_library = source.load('sharedassets32.assets')
    actor_clips = actor_library.objects[104].read_typetree()['clips']
    shot_clips = shot_library.objects[745].read_typetree()['clips'][:2]
    if [(clip['name'], len(clip['frames'])) for clip in actor_clips] != [
        ('Fire', 6), ('Fly', 8), ('Death Air', 6), ('Fire Long', 12), ('TurnToFly', 10)
    ] or [(clip['name'], len(clip['frames'])) for clip in shot_clips] != [('Idle', 4), ('Impact', 6)]:
        raise ValueError('Unexpected Spitter or Spitter Shot R animation library')
    emit_original_clips(args, source, 'spitter', 'spitter-anim.bin', [
        *((actor_library, clip) for clip in actor_clips),
        *((shot_library, dict(clip, name='Shot ' + clip['name'])) for clip in shot_clips),
    ])


def convert_fungus_flyer(args):
    source = Original(args.source_root)
    library = source.load('sharedassets167.assets')
    clips = library.objects[134].read_typetree()['clips']
    if [(clip['name'], len(clip['frames'])) for clip in clips] != [
        ('Death', 5), ('Float', 6), ('Spit Antic', 7), ('Turn', 3),
        ('Spit', 3), ('Spit Recover', 1), ('Effect', 6),
    ]:
        raise ValueError('Unexpected Fungus Flyer animation library')
    smoke = source.load('sharedassets128.assets').objects[7].read()
    if smoke.m_Name != 'smoke_cloud_orange' or smoke.image.size != (118, 109):
        raise ValueError('Unexpected Fungus Flyer gas texture')
    emit_original_clips(args, source, 'fungus-flyer', 'fungus-flyer-anim.bin',
                        [(library, clip) for clip in clips],
                        [('Gas', smoke.image.convert('RGBA'), (-.5, -.5, .5, .5))])


def convert_jellyfish(args):
    source = Original(args.source_root)
    adult = source.load('sharedassets140.assets')
    baby = source.load('sharedassets197.assets')
    adult_clips = adult.objects[165].read_typetree()['clips']
    baby_clips = baby.objects[35].read_typetree()['clips']
    if [(clip['name'], len(clip['frames'])) for clip in baby_clips] != [('Idle', 18)]:
        raise ValueError('Unexpected Jellyfish Baby animation library')
    emit_original_clips(args, source, 'jellyfish', 'jellyfish-anim.bin', [
        *((adult, clip) for clip in adult_clips),
        (baby, dict(baby_clips[0], name='Baby Idle')),
    ])


def convert_fungoon_baby(args):
    source = Original(args.source_root)
    library = source.load('sharedassets167.assets')
    clips = library.objects[180].read_typetree()['clips']
    if [(clip['name'], len(clip['frames'])) for clip in clips] != [
        ('Idle', 6), ('TurnToIdle', 8), ('Death', 5),
    ]:
        raise ValueError('Unexpected Fungoon Baby animation library')
    emit_original_clips(args, source, 'fungoon-baby', 'fungoon-baby-anim.bin',
                        [(library, dict(clip, wrapMode=2) if clip['name'] == 'Death' else clip)
                         for clip in clips])


def convert_fung_crawler(args):
    source = Original(args.source_root)
    library = source.load('sharedassets168.assets')
    clips = library.objects[48].read_typetree()['clips']
    names = {'Walk': ('walk', 4), 'Turn': ('turn', 2),
             'Death Air': ('death', 2), 'Death Land': ('death_land', 3)}
    selected = {clip['name']: clip for clip in clips if clip['name'] in names}
    if set(selected) != set(names) or any(len(selected[name]['frames']) != count
                                          for name, (_, count) in names.items()):
        raise ValueError('Unexpected Fung Crawler animation library')
    selected['Walk'] = dict(selected['Walk'], wrapMode=2)
    selected['Turn'] = dict(selected['Turn'], wrapMode=1)
    emit_original_clips(args, source, 'fung-crawler', 'fung-crawler-anim.bin',
                        [(library, dict(selected[name], name=alias))
                         for name, (alias, _) in names.items()])


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source-root',type=Path,default=room.source_root())
    p.add_argument('--facing',type=Path,default=ROOT/'_private/converted-psp/actor-facing')
    p.add_argument('--out',type=Path,default=ROOT/'_private/converted-psp/anim-libraries')
    p.add_argument('--bank')
    p.add_argument('--from-bank',help='Start a batch at this bank (earlier outputs are untouched)')
    p.add_argument('--scene',action='store_true',help='With --bank: write the scene-enemy render table (SCENE_BANKS)')
    args=p.parse_args()
    # Never write into the original/HK3DS input tree.
    if not args.out.resolve().is_relative_to((ROOT/'_private/converted-psp').resolve()):
        raise ValueError('Library output must stay in this checkout converted-psp')
    if args.bank == 'zombie-guard-anim.bin':
        convert_zombie_guard(args)
        return
    if args.bank == 'spitter-anim.bin':
        convert_spitter(args)
        return
    if args.bank == 'fungus-flyer-anim.bin':
        convert_fungus_flyer(args)
        return
    if args.bank == 'jellyfish-anim.bin':
        convert_jellyfish(args)
        return
    if args.bank == 'fungoon-baby-anim.bin':
        convert_fungoon_baby(args)
        return
    if args.bank == 'fung-crawler-anim.bin':
        convert_fung_crawler(args)
        return
    actors=audit.inventory(args.source_root,args.facing)
    # Greenpath: this checkout's own bank (its snapshot adds the Moss Knight shield, evade, second
    # slash, shoot and Grass Ball clips); the shared source root keeps the older one.
    # Enemy C likewise: its own battle snapshot adds every sentry, Flying Sentry and Vomit Glob clip.
    own = {'greenpath-art.bin': ROOT/'_private/converted/greenpath-enemies/romfs/greenpath-art.bin',
           'enemy-c-anim.bin': ROOT/'_private/converted/enemy-c/enemy-c-anim.bin'}
    actors=[dict(actor, bank=str(own[Path(actor['bank']).name]))
            if Path(actor['bank']).name in own and own[Path(actor['bank']).name].is_file() else actor
            for actor in actors]
    groups={}
    for actor in actors: groups.setdefault(Path(actor['bank']).name,[]).append(actor)
    if args.bank:
        scene_clips = None
        if args.scene:
            if args.bank not in SCENE_BANKS: raise ValueError(f'{args.bank}: no scene render table')
            simulation = (Path(groups[args.bank][0]['bank']) if args.bank == 'greenpath-art.bin'
                          else ROOT/'_private/converted-psp/scene-enemies'/args.bank)
            scene_clips = hka1_clips(simulation)
        convert_bank(args,groups[args.bank],scene_clips);return
    selected=[*groups, 'zombie-guard-anim.bin', 'spitter-anim.bin', 'fungus-flyer-anim.bin',
              'jellyfish-anim.bin', 'fungoon-baby-anim.bin', 'fung-crawler-anim.bin']
    if args.from_bank: selected=selected[selected.index(args.from_bank):]
    for bank in selected:
        base=[sys.executable,'-I',__file__,'--source-root',str(args.source_root),
              '--facing',str(args.facing),'--out',str(args.out),'--bank',bank]
        subprocess.run(base,check=True)
        if bank in SCENE_BANKS: subprocess.run(base+['--scene'],check=True)


if __name__=='__main__':
    main()
