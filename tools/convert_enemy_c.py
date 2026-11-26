"""Pack saved C enemies and battle-wave actors; never contacts the original player."""
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
from animation_library import complete_aliases, event_bytes
from tool_paths import TEX3DS as TEX3DS_EXE

ACTORS = {
    'bully': ('husk_bully_candidate', {'Idle': 'Idle', 'Move': 'Walk', 'Antic': 'Attack Anticipate',
              'Attack': 'Attack Lunge', 'Recover': 'Attack Cooldown', 'Death': 'Death Air', 'Land': 'Death Land'}),
    'baldur': ('baldur_candidate', {'Idle': 'Idle', 'Move': 'Roll', 'Antic': 'Start', 'Recover': 'Stop', 'Death': 'Death Air', 'Land': 'Death Land'}),
    'mother': ('hatcher_actor', {'Idle': 'Fly', 'Attack': 'Fire', 'Death': 'Death Air', 'Land': 'Death Land'}),
    'mawlek': ('brooding_mawlek', {'Idle': 'Body Idle', 'Move': 'Body Walk', 'Spit': 'Dummy Shoot Antic', 'Shoot': 'Dummy Shoot',
              'Jump': 'Dummy Jump', 'Land': 'Dummy Land', 'Swipe': 'Arm Swipe'}),
    'baby': ('hatcher_baby', {'Idle': 'Fly', 'Move': 'Fly', 'Death': 'Death'})
}
BATTLE_ACTORS = {
    'brawler': ('brawler', {'Idle':'Sleep', 'Move':'Run', 'Antic':'Antic', 'Attack':'Smash Loop',
                          'Recover':'Smash to Run', 'Death':'Death Air'}),
    'sentry': ('sentry', {'Idle':'Idle', 'Move':'Run', 'Attack':'Attack 01',
                        'Recover':'Run End', 'Death':'Death Air', 'Land':'Death Land'}),
    'heavy': ('heavy', {'Idle':'Idle', 'Move':'Run', 'Antic':'Charge Antic', 'Attack':'Attack',
                      'Recover':'Charge End', 'Death':'Death Air', 'Land':'Death Land'}),
    'flying': ('flying', {'Idle':'Idle', 'Move':'Idle', 'Antic':'Slash Antic', 'Attack':'Slash',
                        'Recover':'Slash Recover', 'Death':'Death Air', 'Land':'Death Land'}),
    # Vomit Glob (Idle in the air, Land on the ground); no HealthManager, so no profile.
    'glob': ('glob', {}),
    # Shot Mawlek NoDrip (sharedassets32 library 745: Idle, Impact, ...); no profile.
    'shot': ('shot', {})
}
ACTORS.update(BATTLE_ACTORS)


def convert(source, output, romfs=None, tex3ds=Path(TEX3DS_EXE),
            battle_source=ROOT/'_private/converted/battle-enemies-source'):
    for path in (output, romfs):
        if path and not converted_files.private_output(path, ROOT):
            raise ValueError('Enemy source and generated resources must stay private')
    mappings = {root: json.loads((root / 'enemies-map.json').read_text()) for root in (source, battle_source)}
    for root, mapping in mappings.items():
        for name, expected in mapping['source_sha256'].items():
            if hashlib.sha256((root / name).read_bytes()).hexdigest() != expected:
                raise ValueError('Changed enemy source: ' + name)
    output.mkdir(parents=True, exist_ok=True)
    clips, images, configurations = [], {}, []
    atlases, collections = {}, {}
    for prefix, (key, aliases) in ACTORS.items():
        root = battle_source if prefix in BATTLE_ACTORS else source
        mapping = mappings[root]
        actor = mapping['actors'][key]
        library = json.loads((root / actor['animation_libraries'][0]).read_text())['m_Structure']
        available = {c['name']: c for c in library['clips']}
        for alias, original in complete_aliases(library['clips'], aliases).items():
            clip = available[original]
            ids = []
            for frame in clip['frames']:
                ref = frame['spriteCollection']
                collection_name = actor['sprite_collections'][f'{ref["m_FileID"]}:{ref["m_PathID"]}']
                sprite_key = (collection_name, frame['spriteId'])
                if sprite_key not in images:
                    if collection_name not in collections:
                        collections[collection_name]=json.loads((root / collection_name).read_text())['m_Structure']
                        with Image.open(root / actor['textures'][collection_name]) as texture:
                            atlases[collection_name]=texture.convert('RGBA')
                    bitmap,geometry=decode_sprite(atlases[collection_name], collections[collection_name]['spriteDefinitions'][frame['spriteId']])
                    # Original 64 px/unit artwork is shown near 16 px/unit on the top screen.
                    bitmap=bitmap.resize((max(1,round(bitmap.width/4)),max(1,round(bitmap.height/4))),Image.Resampling.LANCZOS)
                    images[sprite_key]=(bitmap,geometry)
                ids.append(sprite_key)
            clips.append((prefix + '/' + alias, clip, ids))
        parts = actor['components']
        def component(kind):
            return json.loads((root / parts[kind][0]).read_text())['m_Structure']
        if 'HealthManager' in parts:
            health, collider = component('HealthManager'), json.loads((root / parts['BoxCollider2D'][0]).read_text())
            size, offset = collider['m_Size'], collider['m_Offset']
            def axis(v, key): return v[key] if key in v else v['m_'+key.upper()]
            x,y,w,h=axis(offset,'x'),axis(offset,'y'),axis(size,'x'),axis(size,'y')
            body = [x-w/2,y-h/2,x+w/2,y+h/2]
            geo = health['smallGeoDrops'] + 5*health['mediumGeoDrops'] + 25*health['largeGeoDrops']
            configurations.append(dict(prefix=prefix, hp=health['hp'], geo=geo, body=body))
    pages, records = [], {}
    page = Image.new('RGBA', (512, 512)); x = y = row = 0
    for key, (bitmap, geometry) in sorted(images.items(), key=lambda item: (-item[1][0].height, -item[1][0].width, item[0])):
        width, height = bitmap.size
        if max(width, height) > 510:
            raise ValueError('Enemy sprite exceeds atlas page')
        if x+width+2 > 512:
            x=0; y+=row; row=0
        if y+height+2 > 512:
            pages.append(page);page=Image.new('RGBA',(512,512));x=y=row=0
        page.paste(bitmap,(x+1,y+1));records[key]=(len(pages),x+1,y+1,width,height,*geometry)
        x+=width+2;row=max(row,height+2)
    pages.append(page)
    if len(pages)>8:
        raise ValueError('Enemy C atlas exceeds its 4 MiB New 3DS budget')
    ids=sorted(images);indices={key:i for i,key in enumerate(ids)}
    with (output/'enemy-c-anim.bin').open('wb') as out:
        out.write(struct.pack('<4s4I',b'HKA1',len(pages),len(ids),len(clips),sum(len(c[2]) for c in clips)))
        for key in ids:out.write(struct.pack('<5I4f',*records[key]))
        first=0
        for name,clip,frames in clips:
            out.write(struct.pack('<48sf4I',name.encode(),clip['fps'],clip['wrapMode'],clip['loopStart'],first,len(frames)))
            first+=len(frames)
        for _,_,frames in clips:
            for key in frames:out.write(struct.pack('<I',indices[key]))
        out.write(event_bytes(clips))
    for i,page in enumerate(pages):
        png=output/f'enemy-c-page-{i}.png';texture=png.with_suffix('.t3x');page.save(png)
        tex3ds_cache.run([str(tex3ds),'-f','rgba5551','-z','none','-o',str(texture),str(png)],check=True,capture_output=True)
    # Generated gameplay dimensions/health remain reviewable without private art.
    by_prefix={c['prefix']:c for c in configurations}
    # Copied build22529139, Crossroads_ShamanTemple Roller 3 (level76:261):
    # HealthManager14798, BoxCollider2D9389, Rigidbody2D8826, Roller FSM14015.
    # Physics2DSettings gravity -60 * gravityScale .8; Max Speed FSM variable11.
    by_prefix['baldur']=dict(prefix='baldur',hp=15,geo=4,body=[-.5625,-.65625,.53125,.4375])
    by_prefix['womb']=dict(prefix='womb',hp=20,geo=0,body=[-.6,-.6,.6,.6])
    def number(value):return f'{float(value):.9g}f' if float(value)%1 else f'{int(value)}.0f'
    lines=['// Generated by tools/convert_enemy_c.py from build 22529139.','const EnemyCProfile profiles[]={']
    for prefix,speed,gravity in [('bully',1.5,30),('baldur',11,48),('mother',2,0),('womb',2,0),('mawlek',3,30),('baby',5,0),
                                 ('brawler',2,0),('sentry',7.5,30),('heavy',7.5,60),('flying',6,0)]:
        c=by_prefix[prefix];body=','.join(number(v) for v in c['body'])
        lines.append(f'    {{"{prefix}",{c["hp"]},{c["geo"]},{{{body}}},{number(speed)},{number(gravity)}}},')
    lines.append('};')
    (ROOT/'include/enemy_c_data.hpp').write_text('\n'.join(lines)+'\n', newline='\n')
    report=dict(build=22529139,source_sha256={str(root):m['source_sha256'] for root,m in mappings.items()},profiles=by_prefix,pages=len(pages),texture_bytes=len(pages)*512*512*2,
                gameplay_source_refs={'baldur': 'build22529139 Crossroads_ShamanTemple level76:261; HealthManager14798, BoxCollider2D9389, Rigidbody2D8826, Roller14015; Physics2DSettings gravity -60'},
                approximations=['Baldur Start/Roll/Stop/Rest, alert and collision/recoil FSM port remains incomplete',
                                'Quarter-resolution RGBA5551 atlases for the 400x240 display', 'Glowing Womb identity pending; no fabricated enemy',
                                'Battle actors use simplified melee choices; Brawler spit, sentry dash/jump/evade and hit effects remain'])
    (output/'manifest.json').write_text(json.dumps(report,indent=2)+'\n')
    if romfs:
        romfs.mkdir(parents=True,exist_ok=True)
        for old in romfs.glob('enemy-c-page-*.t3x'):
            if int(old.stem.rsplit('-',1)[1])>=len(pages):old.unlink()
        for file in output.glob('*.bin'):shutil.copyfile(file,romfs/file.name)
        for i in range(len(pages)):
            file=output/f'enemy-c-page-{i}.t3x';shutil.copyfile(file,romfs/file.name)
    for old in output.glob('enemy-c-page-*'):
        if int(old.stem.rsplit('-',1)[1])>=len(pages):old.unlink()
    print(json.dumps({k:report[k] for k in ('pages','texture_bytes','profiles')}))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,default=ROOT/'_private/research/crossroads-enemies-c')
    parser.add_argument('--output',type=Path,default=ROOT/'_private/converted/enemy-c')
    parser.add_argument('--romfs',type=Path)
    parser.add_argument('--tex3ds',type=Path,default=Path(TEX3DS_EXE))
    args=parser.parse_args();convert(args.source,args.output,args.romfs,args.tex3ds)
