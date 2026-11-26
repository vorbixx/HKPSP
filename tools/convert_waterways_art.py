"""Package original native key-machine/manhole sprites; never synthesize art."""
import converted_files
import argparse
import hashlib
import json
import math
from pathlib import Path
import subprocess

from PIL import Image
from assetripper_client import ROOT
from atlas_packing import pack_pages
from convert_room_visuals import decode
from convert_world_key_doors import require
from convert_world_switch_visuals import bank_bytes
from private_output_paths import is_private_output
from tool_paths import TEX3DS as TEX3DS_EXE

ART = ROOT / '_private/minimum-obstacles/waterways-art'


def native_clip(data, label, resolve):
    require(not data['m_Events'] and not data['m_PositionCurves'] and not data['m_ScaleCurves']
            and not data['m_RotationCurves'], 'Unsupported Waterways animation channels')
    curves = data['m_PPtrCurves']
    require(len(curves) == 1 and curves[0]['m_Attribute'] == 'm_Sprite'
            and curves[0]['m_Path'] == '', 'Unsupported Waterways sprite binding')
    fps = data['m_SampleRate']
    require(math.isfinite(fps) and fps > 0, 'Invalid Waterways native rate')
    keys = curves[0]['m_Curve']
    require(keys and all(abs(k['m_Time'] - i/fps) < 1e-5 for i,k in enumerate(keys)),
            'Unsupported Waterways sprite cadence')
    require(not data['m_FloatCurves'], 'Waterways float curves require a runtime adapter')
    frames = [dict(spriteId=resolve(k['m_Value'])) for k in keys]
    return dict(name=label, fps=fps, wrapMode=2, loopStart=0, frames=frames)


def source_art(art):
    manifest = json.loads((art/'sources.json').read_text())
    require(manifest['scene'] == 'Ruins1_05b', 'Wrong Waterways art source')
    for name, digest in manifest['sha256'].items():
        path = (art/name).resolve()
        require(path.is_relative_to(art.resolve()) and hashlib.sha256(path.read_bytes()).hexdigest() == digest,
                'Changed original Waterways export: ' + name)
    files = sorted(manifest['sprites'])
    def resolve(collection, ref):
        require(not ref['m_FileID'], 'External native sprite curve requires explicit resolution')
        hits = [i for i,f in enumerate(files) if manifest['sprites'][f]['collection'] == collection
                and manifest['sprites'][f]['path_id'] == ref['m_PathID']]
        require(len(hits) == 1, 'Missing/ambiguous native Waterways sprite')
        return hits[0]
    clips = []
    for name, label in (('machine', 'Machine Open'), ('manhole', 'Manhole Open')):
        part = manifest['parts'][name+'_clip']
        data = json.loads((art/part['file']).read_text())
        clips.append(native_clip(data, label, lambda ref: resolve(part['collection'], ref)))
    idle = [files.index(manifest['parts'][name]['file']) for name in
            ('machine_sprite','manhole_sprite','machine_opened','manhole_opened')]
    return manifest, files, clips, idle


def convert(art, output, tex3ds):
    require(is_private_output(output), 'Waterways assets must remain private')
    manifest, files, clips, _ = source_art(art)
    sprites = []
    for i,name in enumerate(files):
        data = json.loads((art/name).read_text())
        ref = data['m_RD']['m_Texture']
        collection = manifest['sprites'][name]['collection']
        require(not ref['m_FileID'], 'External sprite texture requires explicit resolution')
        textures = [f for f,t in manifest['textures'].items()
                    if t['collection'] == collection and t['path_id'] == ref['m_PathID']]
        require(len(textures) == 1, 'Missing/ambiguous native Waterways texture')
        sprites.append((i,*decode(Image.open(art/textures[0]).convert('RGBA'),data)))
    records,pages = pack_pages(sprites,[(1024,1024),(1024,1024)])
    output.mkdir(parents=True,exist_ok=True)
    for i,page in enumerate(pages):
        png = output/f'waterways-art-page-{i}.png';page.save(png)
        subprocess.run([str(tex3ds),'-f','rgba4','-z','none','-o',str(output/f'waterways-art-page-{i}.t3x'),str(png)],check=True)
    (output/'waterways-art.bin').write_bytes(bank_bytes(clips,list(range(len(files))),records,page_count=len(pages)))
    print(f'Waterways original art: {len(files)} sprites, {len(pages)} RGBA4 pages')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--art',type=Path,default=ART)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--tex3ds',type=Path,default=Path(TEX3DS_EXE))
    args = parser.parse_args();convert(args.art,args.output,args.tex3ds)
