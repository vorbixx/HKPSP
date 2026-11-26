"""Pack original conditional-event art and English dialogue for Tutorial_01."""
import converted_files  # Install hardlink copy-on-write before asset writes.
import argparse
import hashlib
import json
import shutil
import struct
import tex3ds_cache
import xml.etree.ElementTree as ET
from pathlib import Path
from PIL import Image
from assetripper_client import ROOT
from convert_hero import decode_sprite
from convert_room_visuals import decode
from atlas_packing import pack_pages
from convert_decor_effects import limits
from conditional_particle_data import compile_particles
from mushroom_dialogue_layout import LAYOUT, wrap_page, write_header
from tool_paths import TEX3DS as TEX3DS_EXE


def convert(source, output, romfs, tex3ds):
    for p in (output, romfs):
        if not p.absolute().is_relative_to(ROOT / '_private/converted'):
            raise ValueError('Converted resources must remain private')
        p.mkdir(parents=True, exist_ok=True)
    m = json.loads((source / 'event-map.json').read_text())
    for name, expected in m['source_sha256'].items():
        if hashlib.sha256((source / name).read_bytes()).hexdigest() != expected:
            raise ValueError('Changed event source: ' + name)
    collections = {k: json.loads((source / k).read_text())['m_Structure'] for k in m['textures']}
    atlases = {k: Image.open(source / v).convert('RGBA') for k, v in m['textures'].items()}
    sprites, clips = {}, []
    def add(key, bitmap, geometry):
        if key not in sprites:
            # Same 32 texels/unit reduction as the existing Shade/Great Door.
            bitmap = bitmap.resize((max(1, round(bitmap.width / 2)), max(1, round(bitmap.height / 2))), Image.Resampling.LANCZOS)
            sprites[key] = (bitmap, geometry)
        return key
    for label, item in m['libraries'].items():
        library = json.loads((source / item['file']).read_text())['m_Structure']
        for c in library['clips']:
            if label=='projectile' and c['name']=='Pillar':
                continue # Level-three flame pillar is outside the Tutorial level-two event.
            keys = []
            for f in c['frames']:
                r = f['spriteCollection']
                ck = item['collections'][f'{r["m_FileID"]}:{r["m_PathID"]}']
                key = f'{ck}/{f["spriteId"]}'
                keys.append(add(key, *decode_sprite(atlases[ck], collections[ck]['spriteDefinitions'][f['spriteId']])))
            clips.append(dict(name=label+' '+c['name'], fps=c['fps'], wrap=c['wrapMode'], loop=c['loopStart'], frames=keys))
    flame = m['native_clips']['Flame']
    frames = []
    for key in flame['frames']:
        image = Image.open(source / m['native_sprites'][key]).convert('RGBA')
        frames.append(add(key, *decode(image, json.loads((source / key).read_text()))))
    clips.append(dict(name='Flame', fps=flame['fps'], wrap=0, loop=0, frames=frames))
    # Dialogue uses the shared LCD-sized LA8 glyphs in the HUD, not scaled
    # copies in the event artwork's RGBA4 pages.
    # Shared DialogueManager artwork, already exported for lore inspection.
    # Reuse the fixed event pages: no additional linear-heap texture allocation.
    dialogue_source = ROOT/'_private/research/lore-tablets'
    dialogue_map = json.loads((dialogue_source/'lore-map.json').read_text())
    for filename in ('resources.assets-21989.json', 'resources.assets-343.png'):
        if hashlib.sha256((dialogue_source/filename).read_bytes()).hexdigest() != dialogue_map['source_sha256'][filename]:
            raise ValueError('Changed dialogue source: ' + filename)
    collection = json.loads((dialogue_source/'resources.assets-21989.json').read_text())['m_Structure']
    texture = Image.open(dialogue_source/'resources.assets-343.png').convert('RGBA')
    for name, sprite_name in (('Top', 'dialogue_fleur_top0008'), ('Bottom', 'dialogue_fleur_bottom0008'),
                              ('Arrow', 'arrow_down0005'), ('Stop', 'fullstop0001')):
        definition = next(s for s in collection['spriteDefinitions'] if s['name'] == sprite_name)
        key = add('dialogue/'+name, *decode_sprite(texture, definition))
        clips.append(dict(name='Dialogue '+name, fps=1, wrap=0, loop=0, frames=[key]))
    # Fixed emitter order shared with ConditionalEvents. Original textures and
    # ranges; the runtime samples one in 32 particles and simplifies the shape.
    owners = ['level6-2122.json','level6-2118.json','level6-2117.json',
              'sharedassets6.assets-551.json','sharedassets6.assets-550.json',
              'sharedassets6.assets-552.json','level6-3790.json','level6-2125.json']
    with (output/'events-fx.bin').open('wb') as f:
        f.write(b'HKF1')
        for index,owner in enumerate(owners):
            entry=m['particles'][owner]
            system=json.loads((source/entry['system']).read_text())
            init=system['m_InitialModule']
            material=json.loads((source/entry['material']).read_text())
            color=init['m_StartColor']['m_MaxColor']
            tint=material['m_SavedProperties']['m_Colors'].get('_TintColor',dict(m_R=1,m_G=1,m_B=1,m_A=1))
            rgba=[color['m_'+c]*tint['m_'+c] for c in 'RGBA']
            image=Image.open(source/entry['texture']).convert('RGBA').resize((64,64),Image.Resampling.LANCZOS)
            # Bake material tint, preserving the original bitmap's shape.
            image=Image.merge('RGBA',tuple(channel.point(lambda v,c=c:round(v*max(0,min(1,rgba[c])))) for c,channel in enumerate(image.split())))
            key='particle/'+str(index)
            sprites[key]=(image,(-.5,-.5,1,1))
            clips.append(dict(name='event particle '+str(index),fps=1,wrap=0,loop=0,frames=[key]))
            transform=json.loads((source/m['actors'][owner]['Transform'][0]).read_text())['m_LocalPosition']
            offset=[transform['m_X'],transform['m_Y']] if index!=6 else [0,0]
            rate=limits(system['m_EmissionModule']['m_RateOverTime'])[1]
            values=[rate,*limits(init['m_StartLifetime']),*limits(init['m_StartSize']),*limits(init['m_StartSpeed']),limits(init['m_GravityModifier'])[1]*-9.81,*offset]
            f.write(struct.pack('<10f',*values))
    extra, particle_records = compile_particles(source, m, sprites, clips)
    (output/'events-fx-extra.bin').write_bytes(extra)
    ids = sorted(sprites)
    records, pages = pack_pages([(k,*sprites[k]) for k in ids], [(1024,1024),(1024,512)])
    with (output / 'events-anim.bin').open('wb') as f:
        f.write(struct.pack('<4s4I', b'HKA1', len(pages), len(ids), len(clips), sum(len(c['frames']) for c in clips)))
        for k in ids:
            f.write(struct.pack('<5I4f', *records[k]))
        first = 0
        for c in clips:
            f.write(struct.pack('<48sf4I', c['name'].encode(), c['fps'], c['wrap'], c['loop'], first, len(c['frames'])))
            first += len(c['frames'])
        for c in clips:
            for k in c['frames']:
                f.write(struct.pack('<I', ids.index(k)))
    for n,page in enumerate(pages):
        page.save(output/f'events-page-{n}.png')
        tex3ds_cache.run([str(tex3ds),'-f','rgba4','-z','none','-o',str(output/f'events-page-{n}.t3x'),str(output/f'events-page-{n}.png')],check=True)
    entries = {e.attrib['name']:e.text for e in ET.parse(source/'EN_Minor NPC.xml').iter('entry')}
    texts = [entries['MR_MUSHROOM_7'], *entries['MR_MUSHROOM_FINAL'].split('<page>'),
             entries['MR_MUSHROOM_FINAL2'], *entries['MR_MUSHROOM_DREAM'].split('<page>'),
             entries['MR_MUSHROOM_SHROOMISH1'], entries['MR_MUSHROOM_SHROOMISH2'], entries['MR_MUSHROOM_SHROOMISH3']]
    font = json.loads((ROOT/'_private/research/tutorial-reminders/prompt-map.json').read_text())['fonts']['sharedassets0.assets-194.json']
    advances = {g['id']:g['xAdvance']*16/font['m_fontInfo']['PointSize'] for g in font['m_glyphInfoList']}
    advances[32] = 4
    write_header()
    with (output/'events-text.bin').open('wb') as f:
        f.write(struct.pack('<4sI', b'HKE1', len(texts)))
        for text in texts:
            text = wrap_page(text, advances)
            raw = text.encode('ascii')
            f.write(struct.pack('<I', len(raw)))
            f.write(raw)
    for name in ('events-anim.bin', 'events-text.bin', 'events-fx.bin', 'events-fx-extra.bin', *(f'events-page-{n}.t3x' for n in range(len(pages)))):
        shutil.copyfile(output/name, romfs/name)
    report = dict(build_id=m['build_id'], source_sha256=m['source_sha256'], pages=len(pages),
                  page_sizes=[list(p.size) for p in pages], texture_format='rgba4',
                  texture_bytes=sum(p.width*p.height*2 for p in pages), sprites=len(ids),
                  clips=[c['name'] for c in clips], text_pages=len(texts), source_particles=particle_records,
                  dialogue_layout=LAYOUT, dialogue_source_sha256={k:dialogue_map['source_sha256'][k] for k in ('resources.assets-21989.json','resources.assets-343.png')},
                  limitations=['Half source raster resolution and RGBA4 quantization', 'Original text adapted to bottom LCD',
                               'Particle curves sampled eight times; mean random start colour; projected 3D motion',
                               'Additive particle contribution encoded as alpha; soft depth intersection omitted'])
    (output/'manifest.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps({k:v for k,v in report.items() if k not in ['source_sha256','clips','source_particles']}))


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, default=ROOT/'_private/research/conditional-events')
    p.add_argument('--output', type=Path, default=ROOT/'_private/converted/conditional-events')
    p.add_argument('--romfs', type=Path, default=ROOT/'_private/converted/hero/romfs')
    p.add_argument('--tex3ds', type=Path, default=Path(TEX3DS_EXE))
    a = p.parse_args()
    convert(a.source, a.output, a.romfs, a.tex3ds)
