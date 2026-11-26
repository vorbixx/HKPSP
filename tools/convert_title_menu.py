"""Prepare private original title artwork, menu glyphs and streamed title music.

Normal conversion is offline; --extract reads the preserved original copy only.
Artwork uses RGBA4 plus an RGB8 backdrop (640 KiB total); audio is streamed.
"""
import argparse
import base64
import hashlib
import json
import math
from pathlib import Path
import shutil
import subprocess
import sys
import wave

sys.path.insert(0, str(Path(__file__).resolve().parent))
import converted_files  # Install hardlink copy-on-write before asset writes.
import tex3ds_cache

from PIL import Image, ImageDraw, ImageFilter, ImageFont, ImageOps
from tool_paths import TEX3DS as TEX3DS_EXE

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / '_private/research/title-menu'
OUTPUT = ROOT / '_private/converted/title-menu'


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024*1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def title_ffmpeg():
    from convert_enemy_audio import FFMPEG
    ffmpeg = FFMPEG if FFMPEG.is_file() else shutil.which('ffmpeg')
    if not ffmpeg:
        raise ValueError('Install ffmpeg or put ffmpeg.exe on PATH to decode the title music')
    return str(ffmpeg)


def extract(source, original, client=None):
    for package in ('audio-python', 'boss-python'):
        sys.path.insert(0, str(ROOT / '_private/tools' / package))
    import UnityPy
    source.mkdir(parents=True, exist_ok=True)
    path = original / 'sharedassets1.assets'
    objects = {o.path_id: o for o in UnityPy.load(str(path)).objects}
    records = []
    for path_id, name, filename in ((121, 'title', 'title-source.png'),
                                     (20, 'Voidheart_menu_BG', 'background-source.png')):
        asset = objects[path_id].read()
        if asset.m_Name != name:
            raise ValueError(f'Unexpected title source at {path_id}')
        asset.image.save(source / filename)
        records.append(dict(collection=path.name, path_id=path_id, name=name, file=filename))
    music = objects[94].read()
    if music.m_Name != 'Title':
        raise ValueError('Unexpected original title music')
    if not (source / 'music-source.wav').exists():
        # AssetRipper's Vorbis rebuild handles this FSB5 better than the local FMOD DLL.
        from assetripper_client import Client
        client = client or Client()
        rows = [r for r in client.search('Title') if r['type']=='AudioClip'
                and r['collection']=='sharedassets1.assets' and r['path']['D']==94]
        if len(rows) != 1:
            raise ValueError('Load copied sharedassets1.assets in local AssetRipper before --extract')
        encoded = client.request('/Assets/Audio', Path=json.dumps(rows[0]['path']))
        (source/'music-source.ogg').write_bytes(encoded)
        subprocess.run([title_ffmpeg(), '-v', 'error', '-y', '-i', str(source/'music-source.ogg'),
            '-acodec', 'pcm_s16le', str(source/'music-source.wav')], check=True)
    records.append(dict(collection=path.name, path_id=94, name='Title', file='music-source.wav'))
    # The HUD already exports this shared original menu typeface.
    hud = ROOT / '_private/research/hud'
    font_key = json.loads((hud / 'hud-map.json').read_text())['font']
    font = json.loads((hud / font_key).read_text())
    if font['m_Name'] != 'TrajanPro-Bold':
        raise ValueError('Unexpected original menu typeface')
    (source / 'font.otf').write_bytes(base64.b64decode(font['m_FontData']))
    records.append(dict(collection=font_key, name='TrajanPro-Bold', file='font.otf'))
    hashes = {path.name: sha(path)}
    for extra in (path.with_suffix('.resource'), original.parent.parent / 'appmanifest_367520.acf'):
        if extra.exists():
            hashes[extra.name] = sha(extra)
    for record in records:
        record['sha256'] = sha(source / record['file'])
    (source / 'provenance.json').write_text(json.dumps(dict(build_id=22529139,
        originals=hashes, assets=records), indent=2) + '\n')


def extract_menu_particles(source, original):
    """Export Classic menu emitters/artwork from the preserved original copy."""
    sys.path.insert(0, str(ROOT / '_private/tools/boss-python'))
    import UnityPy
    scene = UnityPy.load(str(original / 'level1'))
    objects = {o.path_id: o for o in scene.objects}
    emitters = []
    for path_id in (4575, 4639):
        system = objects[path_id].read_typetree()
        initial = system['InitialModule']
        def bounds(value):
            return sorted((value['minScalar'], value['scalar']))
        gradient = system['ColorModule']['gradient']['maxGradient']
        emitters.append(dict(path_id=path_id, count=initial['maxNumParticles'],
            lifetime=bounds(initial['startLifetime']), size=bounds(initial['startSize']),
            emission_rate=system['EmissionModule']['rateOverTime']['scalar'],
            color=initial['startColor']['maxColor'],
            velocity_x=bounds(system['VelocityModule']['x']),
            velocity_z=bounds(system['VelocityModule']['z']),
            fade=[gradient['atime1']/65535, gradient['atime2']/65535]))
    if [e['count'] for e in emitters] != [105, 50]:
        raise ValueError('Original Classic menu emitters changed')
    resources = UnityPy.load(str(original / 'resources.assets'))
    material = next(o for o in resources.objects if o.path_id == 135).read()
    if material.m_Name != 'Particle_Crossroads':
        raise ValueError('Original menu particle material changed')
    pointer = next(v.m_Texture for k, v in material.m_SavedProperties.m_TexEnvs if k == '_MainTex')
    texture = pointer.read()
    if texture.m_Name != 'default_mote' or (texture.m_Width, texture.m_Height) != (29, 39):
        raise ValueError('Original menu particle texture changed')
    source.mkdir(parents=True, exist_ok=True)
    texture.image.save(source / 'menu-particle-source.png')
    data = dict(collection='level1', style='Classic_Style', emitters=emitters,
                material=dict(collection='resources.assets', path_id=135, name=material.m_Name),
                texture=dict(collection='resources.assets', path_id=pointer.m_PathID, name=texture.m_Name))
    (source / 'menu-particles.json').write_text(json.dumps(data, indent=2)+'\n')
    path = source / 'provenance.json'
    provenance = json.loads(path.read_text())
    names = ('menu-particle-source.png', 'menu-particles.json')
    provenance['assets'] = [a for a in provenance['assets'] if a['file'] not in names]
    for name in names:
        provenance['assets'].append(dict(collection='Classic_Style original particle export',
            file=name, name=name, sha256=sha(source/name)))
    for name in ('level1', 'resources.assets'):
        provenance['originals'][name] = sha(original/name)
    stream = getattr(texture, 'm_StreamData', None)
    if stream and stream.path:
        name = Path(stream.path).name
        provenance['originals'][name] = sha(original/name)
    path.write_text(json.dumps(provenance, indent=2)+'\n')
    print('Exported Classic menu bokeh: original default_mote; capacities 105 + 50')


def import_menu_background(source, capture):
    """Preserve a native original-player bake with its capture audit hash."""
    audit = capture.parent / 'source-audit.json'
    with Image.open(capture) as image:
        if image.size != (1920, 1080):
            raise ValueError('Menu background must be a native 1920x1080 source capture')
    audit_hash = sha(audit)
    target = source / 'menu-background-source.png'
    shutil.copy2(capture, target)
    provenance_path = source / 'provenance.json'
    provenance = json.loads(provenance_path.read_text())
    provenance['assets'] = [a for a in provenance['assets'] if a['file'] != target.name]
    provenance['assets'].append(dict(collection='Menu_Title source-player composite',
        name='Blue menu layers; canvas graphics and logos hidden', file=target.name,
        sha256=sha(target), capture_audit_sha256=audit_hash))
    provenance_path.write_text(json.dumps(provenance, indent=2) + '\n')


def import_menu_arrows(source, capture):
    """Preserve the original selection flourish from a native English pause capture."""
    image = Image.open(capture).convert('RGB')
    if image.size != (1920, 1080):
        raise ValueError('Menu arrows require the native 1920x1080 pause reference')
    crop = (800, 436, 840, 468)
    luminance = image.crop(crop).convert('L')
    alpha = luminance.point(lambda value: max(0, min(255, (value-64)*255//176)))
    arrow = Image.new('RGBA', luminance.size, (239, 243, 247, 0))
    arrow.putalpha(alpha)
    source.mkdir(parents=True, exist_ok=True)
    arrow.save(source/'menu-arrow-source.png')
    (source/'menu-arrow-source.json').write_text(json.dumps(dict(
        capture=str(capture), capture_sha256=sha(capture), crop=crop,
        alpha_black=64, alpha_white=240, source_sha256=sha(source/'menu-arrow-source.png')
    ), indent=2)+'\n')


def extract_menu_arrow(source, original):
    """Read the authored Continue-button flourish and fit the existing glyph crop."""
    import UnityPy

    if source.resolve() != source.absolute():
        raise ValueError(f'Menu flourish output must use a direct path: {source}')
    archive = 'sharedassets0.assets'
    path_id = 101
    name = 'main_menu_pointer_anim0010'
    input_path = original / archive
    assets = UnityPy.load(str(input_path))
    record = assets.file.objects[path_id]
    if record.type.name != 'Sprite':
        raise ValueError(f'{archive}:{path_id}: expected a Sprite')
    sprite = record.read()
    if sprite.m_Name != name:
        raise ValueError(f'{archive}:{path_id}: expected {name}, got {sprite.m_Name}')
    # The Continue button's 164x119 RectTransform is scaled by 0.3886591
    # under the 0.7-scale pause screen. Keep the port's 40x32 glyph crop.
    native_size = tuple(math.ceil(value * 0.3886591 * 0.7) for value in (164, 119))
    mask = Image.new('L', (40, 32))
    mask.paste(sprite.image.convert('RGBA').getchannel('A').resize(
        native_size, Image.Resampling.LANCZOS), (0, 2))
    arrow = Image.new('RGBA', mask.size, (239, 243, 247, 0))
    arrow.putalpha(mask)
    target = source / 'menu-arrow-source.png'
    provenance = source / 'menu-arrow-source.json'
    if any(path.is_symlink() for path in (target, provenance)):
        raise ValueError(f'Menu flourish output must use regular files: {source}')
    source.mkdir(parents=True, exist_ok=True)
    arrow.save(target)
    provenance.write_text(json.dumps(dict(
        collection=archive, path_id=path_id, sprite=name, native_size=native_size,
        original_sha256=sha(input_path),
        source_sha256=sha(target)
    ), indent=2)+'\n')


def convert(source, output, romfs, tex3ds, header):
    output.mkdir(parents=True, exist_ok=True)
    provenance = json.loads((source / 'provenance.json').read_text())
    for asset in provenance['assets']:
        if sha(source / asset['file']) != asset['sha256']:
            raise ValueError(f"Source hash changed: {asset['file']}")
    logo = Image.open(source / 'title-source.png').convert('RGBA')
    logo = logo.crop(logo.getchannel('A').getbbox())
    logo.thumbnail((300, 123), Image.Resampling.LANCZOS)
    page = Image.new('RGBA', (512, 128))
    page.alpha_composite(logo, ((300-logo.width)//2, (123-logo.height)//2))
    page.save(output / 'title-logo.png')
    if not any(a['file'] == 'menu-background-source.png' for a in provenance['assets']):
        raise ValueError('Prepare the Menu_Title backdrop before converting the title bank')
    bg = ImageOps.fit(Image.open(source / 'menu-background-source.png').convert('RGBA'),
                      (400, 240), method=Image.Resampling.LANCZOS)
    # Soften the baked shaft's polygon edges at native LCD resolution, offline.
    bg = bg.filter(ImageFilter.GaussianBlur(10))
    page = Image.new('RGBA', (512, 256))
    page.paste(bg, (0, 0))
    page.save(output / 'title-background.png')
    font = ImageFont.truetype(str(source / 'font.otf'), 16)
    page = Image.new('RGBA', (512, 128))
    draw = ImageDraw.Draw(page)
    glyphs = []
    for code in range(32, 127):
        char = chr(code)
        x, y = ((code-32) % 25)*20, ((code-32)//25)*24
        left, top, right, bottom = font.getbbox(char, anchor='ls')
        width, height = right-left, bottom-top
        if height > 24:
            raise ValueError(f'Menu glyph exceeds cell: {char}')
        glyph = Image.new('RGBA', (max(1,width), max(1,height)))
        ImageDraw.Draw(glyph).text((-left,-top),char,font=font,fill='white',anchor='ls')
        if width > 20:
            width = 20  # Q/q swashes exceed the fixed native glyph cell.
            glyph = glyph.resize((width,max(1,height)),Image.Resampling.LANCZOS)
        page.alpha_composite(glyph,(x,y))
        glyphs.append((x,y,width,height,left,top,round(font.getlength(char)*64)))
    arrow_path = source / 'menu-arrow-source.png'
    arrow_source = arrow_path.with_suffix('.json')
    if arrow_source.is_file() and sha(arrow_path) != json.loads(arrow_source.read_text())['source_sha256']:
        raise ValueError('Source hash changed: ' + arrow_path.name)
    arrow = Image.open(arrow_path).convert('RGBA')
    arrow.thumbnail((16, 16), Image.Resampling.LANCZOS)
    page.alpha_composite(arrow, ((16-arrow.width)//2, 104+(16-arrow.height)//2))
    particle = Image.open(source/'menu-particle-source.png').convert('RGBA')
    particle.thumbnail((32, 32), Image.Resampling.LANCZOS)
    particle_box = (32+(32-particle.width)//2, 96+(32-particle.height)//2)
    page.alpha_composite(particle, particle_box)
    page.save(output / 'title-font.png')
    particles = json.loads((source/'menu-particles.json').read_text())
    particle_rows = []
    for emitter in particles['emitters']:
        values = [*emitter['size'], *emitter['lifetime'], *emitter['velocity_x'],
                  *emitter['velocity_z'], *emitter['fade']]
        floats = ', '.join(f'{v:.7f}f' for v in values)
        colors = ', '.join(str(round(emitter['color'][k]*255)) for k in ('r','g','b','a'))
        active_count = min(emitter['count'], round(emitter['emission_rate']*sum(emitter['lifetime'])/2))
        particle_rows.append('    {'+str(active_count)+', '+floats+', '+colors+'}')
    particle_header = ('// Original Classic_Style emitters 4575/4639; default_mote in spare atlas space.\n'
        'struct MoteEmitter { unsigned count; float sizeMin,sizeMax,lifeMin,lifeMax,\n'
        '    vxMin,vxMax,vzMin,vzMax,fadeIn,fadeOut; std::uint8_t r,g,b,a; };\n'
        'inline constexpr MoteEmitter moteEmitters[] = {\n'+',\n'.join(particle_rows)+'\n};\n'
        f'inline constexpr int moteX={particle_box[0]}, moteY={particle_box[1]}, '
        f'moteWidth={particle.width}, moteHeight={particle.height};\n')
    header.parent.mkdir(parents=True, exist_ok=True)
    entries = ',\n'.join('    {'+', '.join(map(str, g))+'}' for g in glyphs)
    header.write_text('// Generated by tools/convert_title_menu.py; original TrajanPro-Bold at 16 px.\n'
        '#pragma once\n#include <cstdint>\nnamespace hk::menu_font {\n'
        '// x/y are atlas pixels; offsets are relative to the text baseline. Advance is 1/64 px.\n'
        'struct Glyph { std::int16_t x, y, width, height, offsetX, offsetY, advance64; };\n'
        'inline constexpr int first = 32, last = 126, lineHeight = 24;\n'
        'inline constexpr Glyph glyphs[] = {\n'+entries+'\n};\n'
        'inline constexpr int glyphWidths[] = {'+', '.join(str(g[2]) for g in glyphs)+'};\n'
        'inline constexpr float glyphAdvance[] = {'+', '.join(str(g[6]/64)+'f' for g in glyphs)+'};\n'+particle_header+'}\n', newline='\n')
    for stem in ('title-logo', 'title-background', 'title-font'):
        # The opaque shaft needs 8-bit channels to preserve its soft gradients.
        texture_format = 'rgb8' if stem == 'title-background' else 'rgba4'
        tex3ds_cache.run([str(tex3ds), '-f', texture_format, '-z', 'none', '-o',
            str(output / (stem+'.t3x')), str(output / (stem+'.png'))], check=True, capture_output=True)
    # Offline resampling only; native runtime streams the result.
    with wave.open(str(source / 'music-source.wav'), 'rb') as wav:
        channels, rate, width = wav.getnchannels(), wav.getframerate(), wav.getsampwidth()
        if width != 2 or channels not in (1, 2):
            raise ValueError('Title music must be PCM16 mono/stereo')
    pcm = subprocess.run([title_ffmpeg(), '-v', 'error', '-i', str(source/'music-source.wav'),
        '-ar', '22050', '-ac', '2', '-f', 's16le', 'pipe:1'], check=True,
        stdout=subprocess.PIPE).stdout
    # Runtime streams canonical 44-byte WAV headers; ffmpeg adds a LIST chunk.
    with wave.open(str(output/'title-music.wav'), 'wb') as wav:
        wav.setparams((2, 2, 22050, 0, 'NONE', 'not compressed'))
        wav.writeframes(pcm)
    with wave.open(str(output/'title-music.wav'), 'rb') as wav:
        frames = wav.getnframes()
    names = ('title-logo.t3x', 'title-background.t3x', 'title-font.t3x', 'title-music.wav')
    result = dict(source=provenance, textures=dict(logo=[512,128], background=[512,256],
        font=[512,128]), logical_logo=[300,123], logical_background=[400,240],
        texture_bytes=655360, music=dict(rate=22050, channels=channels, bits=16,
        frames=frames), output_sha256={n:sha(output/n) for n in names},
        approximations=['Static original blue sprite/beam/particle composite fitted to 400x240.',
                        'Original Classic emission-rate/lifetime populations, capped at 105+50, use a fixed 12 px/unit LCD projection, 0.6 px/unit depth drift, and phase-staggered screen positions.',
                        'Title music resampled to 22050 Hz.'])
    (output / 'title-menu-manifest.json').write_text(json.dumps(result, indent=2)+'\n')
    if romfs:
        romfs.mkdir(parents=True, exist_ok=True)
        for name in (*names, 'title-menu-manifest.json'):
            shutil.copy2(output/name, romfs/name)
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=SOURCE)
    parser.add_argument('--output', type=Path, default=OUTPUT)
    parser.add_argument('--romfs', type=Path)
    parser.add_argument('--extract', action='store_true')
    parser.add_argument('--extract-menu-particles', action='store_true', help='Export original Classic bokeh; requires validation-lock -Exclusive')
    parser.add_argument('--extract-menu-arrow', action='store_true', help='Export the menu flourish directly from Unity assets')
    parser.add_argument('--menu-arrows', type=Path, help='Native English pause capture; import under validation-lock -Exclusive')
    parser.add_argument('--menu-background', type=Path, help='Clean original-player menu-background.png with adjacent source-audit.json')
    parser.add_argument('--original', type=Path, default=ROOT/'_private/original/Hollow Knight/hollow_knight_Data')
    parser.add_argument('--tex3ds', type=Path, default=Path(TEX3DS_EXE))
    parser.add_argument('--header', type=Path, default=ROOT/'include/title_font_data.hpp')
    args = parser.parse_args()
    if args.extract_menu_arrow:
        extract_menu_arrow(args.source, args.original)
        sys.exit(0)
    if args.extract_menu_particles:
        extract_menu_particles(args.source, args.original)
        sys.exit(0)
    if args.extract:
        extract(args.source, args.original)
    if args.menu_background:
        import_menu_background(args.source, args.menu_background)
    if args.menu_arrows:
        import_menu_arrows(args.source, args.menu_arrows)
    result = convert(args.source, args.output, args.romfs, args.tex3ds, args.header)
    print(json.dumps({key:result[key] for key in ('textures','texture_bytes','music')}))
