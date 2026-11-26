"""Convert the exported Tutorial_01 Hollow Shade into its bounded resident bank."""
import converted_files  # Install hardlink copy-on-write before asset writes.
import argparse
import hashlib
import json
import shutil
import struct
import tex3ds_cache
from pathlib import Path
from private_output_paths import is_private_output
from PIL import Image
from assetripper_client import ROOT
from convert_hero import decode_sprite
from convert_room_visuals import decode as decode_native_sprite
from tool_paths import TEX3DS as TEX3DS_EXE

CLIPS = {'Idle', 'Fly', 'Startle', 'TurnToFly', 'Slash Antic', 'Slash',
         'Slash CD', 'Retreat Start', 'Retreat End', 'Death Start', 'Death',
         'Death Glow', 'Slash Effect'}
# Hero Death Anim FindClosest(tag=Shade Marker); level6 _Markers parent
# (61.26,22.69) included. All Spawn Offset FSMs have X=Y=Special Type=0.
MARKERS = [(135.52,45.70),(173.66001,37.849998),(167.05001,65.129995),
           (143.55001,14.01),(111.36,12.44),(98.43,11.94),(93.73,33.80),
           (132.54,64.40),(85.49,45.50),(76.99,62.30)]


def hit_recovery_sprites(source):
    """Targeted, hashed hit prefabs and the recovery child's native sprite."""
    root = source / 'hit-recovery'
    manifest = json.loads((root / 'asset-export-manifest.json').read_text())
    for name, digest in manifest['files_sha256'].items():
        if hashlib.sha256((root / name).read_bytes()).hexdigest() != digest:
            raise ValueError('Changed Shade hit input: ' + name)
    def asset(i):
        return json.loads((root / f'resources.assets-{i}.json').read_text())
    collection = asset(21014)['m_Structure']
    atlas = Image.open(root / 'resources.assets-280.png').convert('RGBA')
    library = asset(21298)['m_Structure']['clips']
    sprites, clips = [], []
    source_clips = {c['name']: c for c in library}
    clip_names = (('Slash Ghost 1', 'Hit Ghost 1'),
                  ('Slash Ghost 2', 'Hit Ghost 2'),
                  ('Slash Shade', 'Hit Slash'))
    if len(source_clips) != len(library) or any(src not in source_clips for src, _ in clip_names):
        raise ValueError('Missing or duplicate Shade hit clips')
    hit_source_clips = [source_clips[src] for src, _ in clip_names]
    ids = sorted({f['spriteId'] for c in hit_source_clips for f in c['frames']})
    ghost_ids = {f['spriteId'] for c in hit_source_clips[:2] for f in c['frames']}
    for i in ids:
        bitmap, geometry = decode_sprite(atlas, collection['spriteDefinitions'][i])
        if i in ghost_ids:
            # GhostDark1/2 tk2dSprite components tint the shared art opaque black.
            alpha = bitmap.getchannel('A')
            bitmap = Image.new('RGBA', bitmap.size)
            bitmap.putalpha(alpha)
        sprites.append((('hit', i), bitmap, geometry))
    for source_name, name in clip_names:
        c = dict(source_clips[source_name], name=name)
        c['frames'] = [dict(_key=('hit', f['spriteId'])) for f in c['frames']]
        clips.append(c)
    for i, name, black in ((3613, 'Hit Flash', True), (3023, 'Recovery Beam', False)):
        sprite = asset(i)
        texture = sprite['m_RD']['m_Texture']['m_PathID']
        bitmap, geometry = decode_native_sprite(Image.open(root / f'resources.assets-{texture}.png').convert('RGBA'), sprite)
        if black:
            # SpriteRenderer16320 is opaque black; preserve its alpha mask.
            alpha = bitmap.getchannel('A')
            bitmap = Image.new('RGBA', bitmap.size)
            bitmap.putalpha(alpha)
        k = ('native', i)
        sprites.append((k, bitmap, geometry))
        clips.append(dict(name=name, fps=1, wrapMode=6, loopStart=0, frames=[dict(_key=k)]))
    return sprites, clips


def presentation_emitters(source):
    """Native idle, retreat and recovery modules, in runtime order."""
    extra = source / 'presentation-remainder'
    hashes = json.loads((extra / 'source_sha256.json').read_text())
    for name in ('idle-retreat-emitters.json', 'native-emitters.json', 'resources.assets-420.png'):
        if hashlib.sha256((extra / name).read_bytes()).hexdigest() != hashes[name]:
            raise ValueError('Changed Shade presentation input: ' + name)
    emitters = json.loads((extra / 'idle-retreat-emitters.json').read_text())
    native = {e['name']: e for e in json.loads((extra / 'native-emitters.json').read_text())}
    for name, transform_file in [('Recovery Shade Particles', 'resources.assets-9402.json'), ('Particle Wave', 'resources.assets-10972.json')]:
        recovery = native[name]['native']
        transform = json.loads((source / 'recovery-children' / transform_file).read_text())
        emitters.append(dict(initial=recovery['m_InitialModule'], emission=recovery['m_EmissionModule'],
                             force=recovery['m_ForceModule'], shape=recovery['m_ShapeModule'], transform=transform,
                             duration=recovery['m_LengthInSec']))
    values = []
    for e in emitters:
        initial, emission, force, shape = (e[k] for k in ('initial', 'emission', 'force', 'shape'))
        def span(c):
            # Native ForceModule endpoints are serialized in descending order.
            # Sorting preserves their uniform distribution without changing it.
            return sorted([c['m_MinScalar'] if c['m_MinMaxState'] == 3 else c['m_Scalar'], c['m_Scalar']])
        offset = e['transform']['m_LocalPosition']
        values += [emission['m_RateOverTime']['m_Scalar'], *span(initial['m_StartLifetime']),
                   *span(initial['m_StartSpeed']), *span(force['m_Y']), offset['m_X'], offset['m_Y'],
                   shape['m_RadiusParameter']['m_Value'], initial['m_StartSize']['m_Scalar'], e['duration']]
    return values


def pack_shade_sprites(sprites):
    """Pack both fixed pages together, retaining a transparent texel gutter."""
    pages = [Image.new('RGBA', (512, 512)), Image.new('RGBA', (512, 256))]
    free = [(i, 0, 0, page.width, page.height) for i, page in enumerate(pages)]
    records = {}
    for key, bitmap, geometry in sorted(sprites, key=lambda s: (-s[1].height, -s[1].width, s[0])):
        w, h = bitmap.width+2, bitmap.height+2
        candidates = [((fw*fh-w*h, min(fw-w, fh-h), p), i)
                      for i, (p, x, y, fw, fh) in enumerate(free) if w <= fw and h <= fh]
        if not candidates:
            raise ValueError('Shade artwork exceeds the compact two-page budget')
        _, index = min(candidates)
        p, x, y, _, _ = free[index]
        pages[p].paste(bitmap, (x+1, y+1))
        records[key] = (p, x+1, y+1, bitmap.width, bitmap.height, *geometry)
        split = []
        for q, fx, fy, fw, fh in free:
            if q != p or x >= fx+fw or x+w <= fx or y >= fy+fh or y+h <= fy:
                split.append((q, fx, fy, fw, fh))
                continue
            if x > fx: split.append((q, fx, fy, x-fx, fh))
            if x+w < fx+fw: split.append((q, x+w, fy, fx+fw-x-w, fh))
            if y > fy: split.append((q, fx, fy, fw, y-fy))
            if y+h < fy+fh: split.append((q, fx, y+h, fw, fy+fh-y-h))
        free = [r for i, r in enumerate(split) if not any(
            i != j and r[0] == o[0] and r[1] >= o[1] and r[2] >= o[2]
            and r[1]+r[3] <= o[1]+o[3] and r[2]+r[4] <= o[2]+o[4]
            for j, o in enumerate(split))]
    return pages, records


def convert(source, output, romfs, tex3ds):
    for p in (output, romfs):
        if not converted_files.private_output(p, ROOT):
            raise ValueError('Original resources must remain private')
        p.mkdir(parents=True, exist_ok=True)
    mapping = json.loads((source / 'shade-map.json').read_text())
    hashes = mapping['source_sha256']
    for name, expected in hashes.items():
        if hashlib.sha256((source / name).read_bytes()).hexdigest() != expected:
            raise ValueError('Changed original input: ' + name)
    library = json.loads((source / mapping['libraries']['shade']).read_text())['m_Structure']
    clips = [c for c in library['clips'] if c['name'] in CLIPS]
    if {c['name'] for c in clips} != CLIPS:
        raise ValueError('Missing Shade clips')
    collections = {n: json.loads((source / n).read_text())['m_Structure'] for n in mapping['textures']}
    atlases = {n: Image.open(source / mapping['textures'][n]).convert('RGBA') for n in collections}

    def key(frame):
        if '_key' in frame:
            return frame['_key']
        ref = frame['spriteCollection']
        return mapping['sprite_collections'][f'{ref["m_FileID"]}:{ref["m_PathID"]}'], frame['spriteId']
    extra_sprites, extra_clips = hit_recovery_sprites(source)
    clips += extra_clips
    ids = sorted({key(f) for c in clips for f in c['frames']})
    extra = {k: (bitmap, geometry) for k, bitmap, geometry in extra_sprites}
    sprites = []
    for k in ids:
        bitmap, geometry = extra[k] if k in extra else decode_sprite(atlases[k[0]], collections[k[0]]['spriteDefinitions'][k[1]])
        # Approximation: hit streaks use 16 texels/unit (quarter source) to
        # retain the two resident pages. Existing artwork stays half source;
        # every source frame, its world geometry and its duration are retained.
        divisor = 4 if k[0] == 'hit' else 2
        bitmap = bitmap.resize((max(1, round(bitmap.width / divisor)), max(1, round(bitmap.height / divisor))), Image.Resampling.LANCZOS)
        sprites.append((k, bitmap, geometry))
    # Original particle_01 (material150), kept in the existing resident page.
    particle_key = ('particle', 0)
    sprites.append((particle_key, Image.open(source / 'presentation-remainder/resources.assets-420.png').convert('RGBA'), (-.5, -.5, .5, .5)))
    sprites.sort(key=lambda s: (-s[1].height, -s[1].width, s[0]))
    pages, records = pack_shade_sprites(sprites)
    with (output / 'shade-anim.bin').open('wb') as out:
        out.write(struct.pack('<4s4I', b'HKA1', len(pages), len(ids)+1, len(clips)+1, sum(len(c['frames']) for c in clips)+1))
        for k in [*ids, particle_key]:
            out.write(struct.pack('<5I4f', *records[k]))
        first = 0
        for c in clips:
            out.write(struct.pack('<48sf4I', c['name'].encode(), c['fps'], c['wrapMode'], c['loopStart'], first, len(c['frames'])))
            first += len(c['frames'])
        out.write(struct.pack('<48sf4I', b'Shade Particle', 1, 6, 0, first, 1))
        for c in clips:
            for f in c['frames']:
                out.write(struct.pack('<I', ids.index(key(f))))
        out.write(struct.pack('<I', len(ids)))
    for i, page in enumerate(pages):
        page.save(output / f'shade-page-{i}.png')
        tex3ds_cache.run([str(tex3ds), '-f', 'rgba4', '-z', 'none', '-o', str(output / f'shade-page-{i}.t3x'), str(output / f'shade-page-{i}.png')], check=True)
    facts = json.loads((source / 'collider-presentation-facts.json').read_text())
    body = facts['resources.assets:13756']; size = body['m_Size']; offset = body['m_Offset']
    bounds = [offset['m_X'] - size['m_X']/2, offset['m_Y'] - size['m_Y']/2,
              offset['m_X'] + size['m_X']/2, offset['m_Y'] + size['m_Y']/2]
    radius = facts['resources.assets:13588']['m_Radius']
    transform = facts['resources.assets:9321']; points = facts['resources.assets:13642']['m_Points']['m_Paths'][0]
    points = [(p['m_X']*transform['m_LocalScale']['m_X']+transform['m_LocalPosition']['m_X'],
               p['m_Y']+transform['m_LocalPosition']['m_Y']) for p in points]
    slash = [min(p[0] for p in points), min(p[1] for p in points), max(p[0] for p in points), max(p[1] for p in points)]
    values = bounds + [-radius,-radius,radius,radius] + slash + [v for p in MARKERS for v in p] + [v for p in points for v in p]
    values += presentation_emitters(source)
    (output / 'shade.bin').write_bytes(b'HKS2' + struct.pack('<'+'f'*len(values), *values))
    for name in ('shade-anim.bin', 'shade.bin', *(f'shade-page-{i}.t3x' for i in range(len(pages)))):
        shutil.copyfile(output / name, romfs / name)
    hit_hashes = json.loads((source/'hit-recovery/asset-export-manifest.json').read_text())['files_sha256']
    report = dict(build_id=22529139, source_sha256={**hashes, **{f'hit-recovery/{n}': h for n, h in hit_hashes.items()}}, pages=len(pages), page_sizes=[list(p.size) for p in pages],
                  texture_format='rgba4', texture_bytes=sum(p.width*p.height*2 for p in pages),
                  sprites=len(ids)+1, clips=[c['name'] for c in clips]+['Shade Particle'], markers=MARKERS,
                  limitations=['Existing sprites half source; hit streaks quarter source; RGBA4 quantization',
                               'Particle RNG differs; planar circle births and analytical constant force',
                               'Hit cone uses a planar projection and independent cosmetic RNG',
                               'Beam uses source-over without its full Sprites/Lit lighting/bloom',
                               'Audio mixer snapshots remain incomplete'])
    (output / 'manifest.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps({k:v for k,v in report.items() if k!='source_sha256'}))


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, default=ROOT/'_private/research/hollow-shade')
    p.add_argument('--output', type=Path, default=ROOT/'_private/converted/shade')
    p.add_argument('--romfs', type=Path, default=ROOT/'_private/converted/hero/romfs')
    p.add_argument('--tex3ds', type=Path, default=Path(TEX3DS_EXE))
    a = p.parse_args(); convert(a.source, a.output, a.romfs, a.tex3ds)
