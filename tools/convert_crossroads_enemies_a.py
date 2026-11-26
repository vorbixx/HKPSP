"""Build private Crossroads A settings and original tk2d animation pages.

Movement approximations are explicit in the manifest; source pixels, frame
geometry and frame rates come from the preserved copied-build exports.
"""
import converted_files  # Install hardlink copy-on-write before asset writes.
import argparse
import copy
import hashlib
import json
import shutil
import struct
import tex3ds_cache
from pathlib import Path
from private_output_paths import is_private_output

from PIL import Image
from assetripper_client import ROOT
from atlas_packing import pack_pages
from convert_hero import decode_sprite
from animation_library import complete_aliases, event_bytes
from tool_paths import TEX3DS as TEX3DS_EXE

STEMS = ('tiktik', 'gruzzer', 'vengefly', 'aspid-hatchling')


def rect(box):
    o, s = box['m_Offset'], box['m_Size']
    return [o['m_X']-s['m_X']/2, o['m_Y']-s['m_Y']/2,
            o['m_X']+s['m_X']/2, o['m_Y']+s['m_Y']/2]


def settings(source, mapping, kind):
    gravity = abs(json.loads((ROOT/'_private/research/assets/Physics2DSettings.json').read_bytes())['m_Gravity']['m_Y'])
    if kind == 2:
        raw = (ROOT/'_private/converted/tutorial/room-entities.bin').read_bytes()
        if raw[:4] != b'RME1' or not struct.unpack_from('<I', raw, 4)[0]:
            raise ValueError('Vengefly requires converted original Buzzer configuration')
        flight = list(struct.unpack_from('<25f', raw, 12))
        hp, damage, layer, geo = struct.unpack_from('<4I', raw, 112)
        return [kind, hp, damage, layer, geo], [*flight, *flight[2:6], flight[17], 12., gravity*.8, 20.]
    def component(name, default=None):
        paths = mapping['actor'].get(name, [])
        if not paths:
            if default is not None:
                return default
            raise ValueError('Missing component '+name)
        d = json.loads((source/paths[0]).read_bytes())
        return d.get('m_Structure', d)
    hp, damage = component('HealthManager'), component('DamageHero')
    recoil = component('Recoil', {})
    death = component('EnemyDeathEffects', {})
    body = rect(component('BoxCollider2D'))
    hero_map = json.loads((ROOT/'_private/research/enemy/crawler-map.json').read_bytes())
    hero = rect(json.loads((ROOT/'_private/research/enemy'/hero_map['hero_box']['BoxCollider2D'][0]).read_bytes()))
    speed, acceleration = 2., 12.
    if kind == 0:
        speed = component('Climber', {}).get('speed', 2.)
    if kind == 1:
        for path in mapping['actor'].get('PlayMakerFSM', []):
            f = json.loads((source/path).read_bytes())['m_Structure']['fsm']
            for v in f['variables']['floatVariables']:
                if v['name'] == 'Speed':
                    speed = v['value']
    if kind == 3:
        speed, acceleration = 5., 5.  # ChaseObject .1 per 50 Hz fixed update.
    # Shared source HeroBox, injury and death layers. Remaining absent flying
    # parameters are presentation/steering approximations, not source evidence.
    flight = [0., 0., *body, *hero, 12., 0., .5, 1., 1., 10., 2.,
              speed, .2, 2., .25, recoil.get('recoilSpeedBase', 15.),
              recoil.get('recoilDuration', .15), .2, .2]
    hp_value, damage_value, layer, geo = hp['hp'], damage['damageDealt'], 11, hp['smallGeoDrops']
    corpse = body
    parts = (mapping.get('corpse') or {}).get('BoxCollider2D', [])
    if parts:
        corpse = rect(json.loads((source/parts[0]).read_bytes()))
    values = [*flight, *corpse, speed, acceleration, gravity*.8, death.get('corpseFlingSpeed', 20.)]
    return [kind, hp_value, damage_value, layer, geo], values


def artwork(source, mapping, kind, output, romfs, tex3ds):
    library = mapping['library']
    if isinstance(library, list):
        library = library[0]['file']
    clips = json.loads((source/library).read_bytes())['m_Structure']['clips']
    by_name = {c['name']: c for c in clips}
    aliases = {}  # Native playback uses source names; no synthetic idle/death clips.
    selected = []
    for name, original in complete_aliases(clips,{a:c['name'] for a,c in aliases.items()}).items():
        source_clip=by_name[original]
        c = copy.deepcopy(source_clip);c['name'] = name;selected.append(c)
    collections = {n: json.loads((source/n).read_bytes())['m_Structure'] for n in set(mapping['sprite_collections'].values())}
    atlases = {n: Image.open(source/mapping['textures'][n]).convert('RGBA') for n in collections}
    def key(frame):
        r = frame['spriteCollection']
        ref = f'{r["m_FileID"]}:{r["m_PathID"]}'
        collections_map = mapping['sprite_collections']
        return collections_map.get(ref, collections_map.get('clip:'+ref)), frame['spriteId']
    ids = sorted({key(f) for c in selected for f in c['frames']})
    sprites = [(k, *decode_sprite(atlases[k[0]], collections[k[0]]['spriteDefinitions'][k[1]])) for k in ids]
    for size in ((256, 256), (256, 512), (512, 512), (512, 1024)):
        try:
            records, pages = pack_pages(sprites, [size]);break
        except ValueError:
            continue
    else:
        raise ValueError('Enemy artwork exceeds one 2 MiB page')
    stem = STEMS[kind]
    with (output/(stem+'-anim.bin')).open('wb') as f:
        f.write(struct.pack('<4s4I', b'HKA1', 1, len(ids), len(selected), sum(len(c['frames']) for c in selected)))
        for k in ids:
            f.write(struct.pack('<5I4f', *records[k]))
        first = 0
        for c in selected:
            f.write(struct.pack('<48sf4I', c['name'].encode(), c['fps'], c['wrapMode'], c['loopStart'], first, len(c['frames'])))
            first += len(c['frames'])
        for c in selected:
            for frame in c['frames']:
                f.write(struct.pack('<I', ids.index(key(frame))))
        f.write(event_bytes([(c['name'],c,c['frames']) for c in selected]))
    png = output/(stem+'-page-0.png');pages[0].save(png)
    # Half-size native RGBA4 pages keep a single arena kind within 1 MiB.
    tex3ds_cache.run([str(tex3ds), '-f', 'rgba4', '-z', 'none', '-o', str(png.with_suffix('.t3x')), str(png)], check=True)
    for suffix in ('-anim.bin', '-page-0.t3x', '.bin'):
        shutil.copyfile(output/(stem+suffix), romfs/(stem+suffix))
    return {'aliases': {name: c['name'] for name, c in aliases.items()},
            'texture_bytes': pages[0].width*pages[0].height*2, 'texture_format': 'rgba4', 'sprites': len(ids)}


def convert(source, output, romfs, tex3ds):
    for path in (output, romfs):
        if not converted_files.private_output(path, ROOT):
            raise ValueError('Enemy assets must remain private')
        path.mkdir(parents=True, exist_ok=True)
    reports = {}
    for kind, stem in enumerate(STEMS):
        folder = ROOT/'_private/research/buzzer' if kind == 2 else source
        map_path = folder/('buzzer-map.json' if kind == 2 else stem+'-map.json')
        mapping = json.loads(map_path.read_bytes())
        for name, digest in mapping['source_sha256'].items():
            if hashlib.sha256((folder/name).read_bytes()).hexdigest() != digest:
                raise ValueError('Changed enemy source '+name)
        integers, values = settings(folder, mapping, kind)
        (output/(stem+'.bin')).write_bytes(struct.pack('<4s5I33f', b'XEA1', *integers, *values))
        reports[stem] = dict(settings=integers, parameters=values, source_sha256=mapping['source_sha256'],
                             **artwork(folder, mapping, kind, output, romfs, tex3ds))
    (output/'manifest.json').write_text(json.dumps(reports, indent=2)+'\n')
    print(json.dumps({k: {'settings': v['settings'], 'texture_bytes': v['texture_bytes']} for k, v in reports.items()}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT/'_private/research/crossroads-enemies-a')
    parser.add_argument('--output', type=Path, default=ROOT/'_private/converted/crossroads-enemies-a')
    parser.add_argument('--romfs', type=Path, default=ROOT/'_private/converted/hero/romfs')
    parser.add_argument('--tex3ds', type=Path, default=Path(TEX3DS_EXE))
    args = parser.parse_args();convert(args.source, args.output, args.romfs, args.tex3ds)
