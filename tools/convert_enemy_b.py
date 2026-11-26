"""Convert copied Crossroads enemy exports; no live-game or Steam-install access."""
import converted_files  # Install hardlink copy-on-write before asset writes.
import argparse
import hashlib
import json
import shutil
import struct
import tex3ds_cache
from animation_library import event_bytes
from pathlib import Path
from private_output_paths import is_private_output

from PIL import Image
from atlas_packing import pack_pages
from convert_hero import decode_sprite
from tool_paths import TEX3DS as TEX3DS_EXE

ROOT = Path(__file__).resolve().parents[1]
GROUPS = ('wandering_husk', 'husk_hornhead', 'leaping_husk', 'goam')
PREFIXES = ('wandering', 'hornhead', 'leaping', 'goam')


def rect(collider):
    offset, size = collider['m_Offset'], collider['m_Size']
    return [offset['m_X']-size['m_X']/2, offset['m_Y']-size['m_Y']/2,
            offset['m_X']+size['m_X']/2, offset['m_Y']+size['m_Y']/2]


def observe(source):
    mapping = json.loads((source/'enemies-map.json').read_text())
    for name, expected in mapping['source_sha256'].items():
        if hashlib.sha256((source/name).read_bytes()).hexdigest() != expected:
            raise ValueError('Changed original export: '+name)
    def read(name):
        return json.loads((source/name).read_text())
    def component(entry, kind):
        return read(entry['parts'][kind][0])
    configs, clips, sprites, goam_shapes = [], [], {}, []
    for index, (group, prefix) in enumerate(zip(GROUPS, PREFIXES)):
        entry = mapping[group]
        library = read(entry['libraries'][0])['m_Structure']['clips']
        original = {c['name']: c for c in library}
        collections = {name: read(name)['m_Structure'] for name in entry['sprite_collections'].values()}
        atlases = {name: Image.open(source/entry['textures'][name]).convert('RGBA') for name in collections}
        def key(frame):
            ref = frame['spriteCollection']
            return (entry['sprite_collections'][f'{ref["m_FileID"]}:{ref["m_PathID"]}'], frame['spriteId'])
        for c in library:
            for frame in c['frames']:
                k = key(frame)
                if k not in sprites:
                    sprites[k] = decode_sprite(atlases[k[0]], collections[k[0]]['spriteDefinitions'][k[1]])
        aliases = {'idle': 'Idle', 'walk': 'Walk', 'turn': 'Turn', 'windup': 'Attack Anticipate',
                   'attack': 'Attack Lunge', 'land': 'Attack Cooldown', 'death': 'Death Air',
                   'death_land': 'Death Land', 'air': 'Fall'}
        if index == 2:
            aliases.update(windup='Attack', attack='Attack', air='Attack', land='Land')
        if index == 3:
            aliases = dict(idle='Down', rise='Burst', exposed='Idle', retract='Retract')
            for name in ('Burst', 'Idle', 'Retract'):
                for frame in original[name]['frames']:
                    k = key(frame)
                    center, half = collections[k[0]]['spriteDefinitions'][k[1]]['colliderVertices']
                    goam_shapes.append([center['x']-half['x'], center['y']-half['y'],
                                        center['x']+half['x'], center['y']+half['y']])
        for name, c in [(c['name'], c) for c in library]+[(a, original[b]) for a, b in aliases.items() if b in original]:
            clips.append(dict(name=prefix+'/'+name, fps=c['fps'], wrap=c['wrapMode'], loop=c['loopStart'],
                              keys=[key(f) for f in c['frames']], events=[{k: v for k, v in f.items()
                              if k not in ('spriteCollection', 'spriteId')} for f in c['frames']]))
        body = rect(component(entry, 'BoxCollider2D'))
        damage = component(entry, 'DamageHero')['m_Structure']['damageDealt']
        if index < 3:
            health = component(entry, 'HealthManager')['m_Structure']
            walker = component(entry, 'Walker')['m_Structure']
            recoil = component(entry, 'Recoil')['m_Structure']
            fsm = read(entry['fsms'][0])
            variables = {v['name']: v['value'] for v in fsm['variables']['floatVariables']}
            # Scaled child trigger geometry, independent of actor facing.
            supplement = read('supplement.json')[group]['children']['Attack Range']
            attack = read(supplement['BoxCollider2D'][0])
            transform = read(supplement['Transform'][0])
            detect_x = attack['m_Size']['m_X']*abs(transform['m_LocalScale']['m_X'])/2
            detect_y = attack['m_Size']['m_Y']*abs(transform['m_LocalScale']['m_Y'])/2
            windup = 3/original['Attack']['fps'] if index == 2 else len(original['Attack Anticipate']['frames'])/original['Attack Anticipate']['fps']
            integers = [health['hp'], damage, health['smallGeoDrops']]
            values = body+[abs(walker['walkSpeedL']), variables.get('Lunge Speed', 0), detect_x, detect_y,
                           windup, variables.get('Jump X Speed', 0), 20 if index == 2 else 0,
                           component(entry, 'Rigidbody2D')['m_GravityScale'], recoil['recoilSpeedBase'],
                           recoil['recoilDuration'], 1, 5/15, 5/12, 1]
        else:
            integers = [0, damage, 0]
            values = body+[0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 5/15, 5/12, 1]
        assert len(values) == 18
        configs.append((integers, values))
    if len(goam_shapes) != 13:
        raise ValueError('Changed Goam animation frame counts')
    return mapping, configs, clips, sprites, goam_shapes


def convert(source, output, romfs, tex3ds):
    for target in (output, romfs):
        if not converted_files.private_output(target, ROOT):
            raise ValueError('Enemy resources must stay private')
        target.mkdir(parents=True, exist_ok=True)
    mapping, configs, clips, sprites, goam_shapes = observe(source)
    ids = sorted(sprites)
    # 24 texels/unit matches the arena display density and fits the 2 MiB budget.
    resolution = .375
    reduced = [(k, sprites[k][0].resize(tuple(max(1, round(x*resolution)) for x in sprites[k][0].size),
                Image.Resampling.LANCZOS), sprites[k][1]) for k in ids]
    records, pages = pack_pages(reduced, [(1024, 512)])
    with (output/'enemy-b.bin').open('wb') as out:
        out.write(b'HKB1')
        for integers, values in configs:
            out.write(struct.pack('<3I18f', *integers, *values))
        for shape in goam_shapes:
            out.write(struct.pack('<4f', *shape))
    with (output/'enemy-b-anim.bin').open('wb') as out:
        out.write(struct.pack('<4s4I', b'HKA1', 1, len(ids), len(clips), sum(len(c['keys']) for c in clips)))
        for k in ids:
            out.write(struct.pack('<5I4f', *records[k]))
        first = 0
        for c in clips:
            out.write(struct.pack('<48sf4I', c['name'].encode(), c['fps'], c['wrap'], c['loop'], first, len(c['keys'])))
            first += len(c['keys'])
        for c in clips:
            for k in c['keys']:
                out.write(struct.pack('<I', ids.index(k)))
        out.write(event_bytes([(c['name'],dict(frames=c['events']),c['keys']) for c in clips]))
    pages[0].save(output/'enemy-b-page-0.png')
    tex3ds_cache.run([str(tex3ds), '-f', 'rgba8', '-z', 'none', '-o', str(output/'enemy-b-page-0.t3x'),
                    str(output/'enemy-b-page-0.png')], check=True)
    for name in ('enemy-b.bin', 'enemy-b-anim.bin', 'enemy-b-page-0.t3x'):
        shutil.copyfile(output/name, romfs/name)
    report = dict(source_sha256=mapping['source_sha256'], configs=configs, goam_shapes=goam_shapes,
                  clips=clips, sprite_keys=ids, records=[records[k] for k in ids], resolution=resolution,
                  texture_bytes=1024*512*4)
    (output/'manifest.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(dict(enemies=4, sprites=len(ids), texture_bytes=report['texture_bytes'], resolution=resolution)))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT/'_private/research/crossroads-enemies-b')
    parser.add_argument('--output', type=Path, default=ROOT/'_private/converted/enemy-b')
    parser.add_argument('--romfs', type=Path, default=ROOT/'_private/converted/hero/romfs')
    parser.add_argument('--tex3ds', type=Path, default=Path(TEX3DS_EXE))
    args = parser.parse_args()
    convert(args.source, args.output, args.romfs, args.tex3ds)
