"""Build source-validated private Tutorial_01 lighting (HKL4 + RGBA8 pages)."""
import converted_files  # Install hardlink copy-on-write before asset writes.
import argparse
import hashlib
import json
import shutil
import struct
import tex3ds_cache
from pathlib import Path
from PIL import Image
from assetripper_client import ROOT
from convert_room_visuals import decode
from convert_tutorial import ORIGINAL_DATA, TUTORIAL_INPUTS, validate_original
from tool_paths import TEX3DS as TEX3DS_EXE

LIGHTING_INPUTS = {
    **TUTORIAL_INPUTS,
    'resources.assets': '6d49ac5a50a282a645acc32518b28e7a29d0b94a0c3b5a4cda18dade3e6fe511',
}
# Settled HeroLight / Vignette placement, Darkness Border bounds in vignette
# coordinates, and VividLight placement reviewed for Steam build 22529139.
# The serialized prefab alone lacks the runtime scale and tint overrides.
SETTLED_LIGHT = (3.0, -0.5999999046325684, 0.03519999980926514,
                 0.6150000095367432, -0.5799999237060547, -0.9959999918937683)
SETTLED_BORDERS = (
    (-39.79145431518555, -43.725852966308594, 26.191450119018555, -10.474151611328125),
    (-39.79145431518555, 10.574149131774902, 26.191450119018555, 43.82584762573242),
    (-82.19145202636719, -47.083526611328125, -16.20854949951172, 47.68352508544922),
    (16.328548431396484, -47.08352279663086, 82.31145477294922, 47.68352508544922),
)
SETTLED_GLOW = (4.5227484703063965, 4.522748947143555, 0.0961456298828125,
                -0.02965545654296875, 0.004000000189989805, 1.0)


def linear_light(image):
    """Preserve signed shader input; alpha-test belongs after runtime tinting."""
    return image.convert('RGBA').copy()


def vivid_light_gain(image):
    """Bake VividLight's upper branch G/(2-2*S) as gain/4 for PICA.

    Source donut RGB is 215..217 wherever alpha survives the .01 cutoff.
    Reject burn-branch or >4 gains rather than silently changing the shader.
    """
    pixels=[]
    for r,g,b,a in image.convert('RGBA').getdata():
        if a>=3 and (min(r,g,b)<=127 or max(r,g,b)>223):
            raise ValueError('Hero glow exceeds the VividLight gain contract')
        pixels.append(tuple(round(min(4.,255/(2*(255-c))) * 255/4) if c<255 else 255
                            for c in (r,g,b))+(a,))
    result=Image.new('RGBA',image.size);result.putdata(pixels)
    # Approximation: half resolution for the broad, smooth gain/alpha texture.
    return result.resize(((image.width+1)//2,(image.height+1)//2),Image.Resampling.BILINEAR)


def hero_damage_tween(control):
    """Keep the saved HeroLight FSM's scaled, linear tween contract explicit."""
    assert control['name'] == 'HeroLight Control'
    vectors = {v['name']: v['value'] for v in control['variables']['vector3Variables']}
    for vector in vectors.values():
        assert vector['x'] == vector['y'] == vector['z'] and vector['x'] > 0
    times = []
    for state in ('Damage', 'Recover', 'Death'):
        actions = next(s['actions'] for s in control['states'] if s['name'] == state)
        actions = [a for a in actions if a.get('enabled', True)]
        assert len(actions) == 1 and actions[0]['type'].endswith('.iTweenScaleTo')
        p = actions[0]['parameters']
        assert p['easeType'] == 21 and p['loopType'] == 0  # iTween.EaseType.linear
        assert not p['realTime']['value'] and p['delay']['value'] == 0
        assert p['stopOnExit']['value'] and p['time']['value'] > 0
        vector_scale = p['vectorScale']
        variable = (vector_scale['variable'] if 'variable' in vector_scale
                    else bytes(vector_scale['bytes'])[13:].decode())
        assert variable == ('Idle Scale' if state == 'Recover' else 'Damage Scale')
        times.append(p['time']['value'])
    pause = next(s for s in control['states'] if s['name'] == 'Damage Pause')['actions']
    assert len(pause) == 1 and pause[0]['type'].endswith('.Wait')
    assert not pause[0]['parameters']['realTime']
    hold = pause[0]['parameters']['time']['value']
    assert hold > 0
    return [vectors['Damage Scale']['x'], times[0], hold, times[1], times[2]]


def vignette_format(image, compact):
    if compact:
        if any(image.getextrema()[channel] != (0, 0) for channel in range(3)):
            raise ValueError('Compact vignette must be black')
        return 'la8'
    return 'rgba8'


def probe_placement(raw, solid):
    rows=[json.loads(s) for s in raw.splitlines()]
    light=next(r for r in rows if r['path']=='Knight/HeroLight')
    vignette=next(r for r in rows if r['path']=='Knight/Vignette')
    hero=next(r for r in rows if r['path']=='Knight')
    assert light['active'] and light['enabled'] and light['matrix'][0:2]==[-3.,0.]
    assert light['matrix'][4]==3. and vignette['sortingLayer']==13 and vignette['order']==0
    hero_y=hero['matrix'][5]
    transform=[abs(light['matrix'][0]),light['matrix'][5]-hero_y,light['matrix'][6],light['color'][3],vignette['matrix'][5]-hero_y,vignette['matrix'][6]]
    half=solid['m_Rect']['m_Width']/solid['m_PixelsToUnits']/2
    borders=[]
    for suffix in ['black_solid','black_solid (1)','black_solid (2)','black_solid (3)']:
        row=next(r for r in rows if r['path']=='Knight/Vignette/Darkness Border/'+suffix)
        m=row['matrix'];vm=vignette['matrix']
        x=(m[2]-vm[2])/vm[0];y=(m[5]-vm[5])/vm[4]
        w=abs(m[0]/vm[0])*half;h=abs(m[4]/vm[4])*half
        borders.append((x-w,y-h,x+w,y+h))
    glow=next(r for r in rows if r['path']=='Knight/white_light_donut')
    assert glow['active'] and glow['enabled'] and glow['shader']=='UI/BlendModes/VividLight'
    gm=glow['matrix'];hm=hero['matrix']
    glow_transform=[abs(gm[0]),gm[4],(gm[2]-hm[2])*(-1 if hm[0]<0 else 1),gm[5]-hm[5],gm[6],glow['color'][3]]
    return transform, borders, glow_transform


def convert(source,output,romfs,tex3ds,probe=None,compact_vignette=False,original=ORIGINAL_DATA):
    inputs={}
    def read(name,directory=source):
        path=directory/(name+'.json');inputs[str(path)]=hashlib.sha256(path.read_bytes()).hexdigest()
        return json.loads(path.read_text())
    if probe is None:
        validate_original(original, LIGHTING_INPUTS)
        inputs.update({str(original / name): signature for name, signature in LIGHTING_INPUTS.items()})
        transform,borders,glow_transform=SETTLED_LIGHT,SETTLED_BORDERS,SETTLED_GLOW
    else:
        raw=probe.read_bytes()
        inputs[str(probe)]=hashlib.sha256(raw).hexdigest()
        transform,borders,glow_transform=probe_placement(raw, read('resources.assets-2862'))
    output.mkdir(parents=True,exist_ok=True);romfs.mkdir(parents=True,exist_ok=True)
    # Shader is GrabPass + source-alpha blending; require the inspected variant.
    shader=read('resources.assets-487')
    blend=shader['m_ParsedForm']['m_SubShaders'][0]['m_Passes'][1]['m_State']['m_RtBlend0']
    assert blend['m_SourceBlend']['m_Value']==5 and blend['m_DestinationBlend']['m_Value']==10
    glow_source=source.parent/'hero-light-donut'
    glow_shader=read('resources.assets-512',glow_source)
    glow_blend=glow_shader['m_ParsedForm']['m_SubShaders'][0]['m_Passes'][1]['m_State']['m_RtBlend0']
    assert glow_blend['m_SourceBlend']['m_Value']==5 and glow_blend['m_DestinationBlend']['m_Value']==10
    assert read('resources.assets-81',glow_source)['m_SavedProperties']['m_Colors']['_Color']==dict(m_R=1,m_G=1,m_B=1,m_A=1)
    binary=bytearray(b'HKL4');sprites=[]
    for name,ident in [('hero-light',3023),('vignette',2642),('hero-glow',3318)]:
        directory=glow_source if name=='hero-glow' else source
        sprite=read(f'resources.assets-{ident}',directory)
        path=directory/f"resources.assets-{sprite['m_RD']['m_Texture']['m_PathID']}.png"
        inputs[str(path)]=hashlib.sha256(path.read_bytes()).hexdigest()
        image,bounds=decode(Image.open(path).convert('RGBA'),sprite)
        if name=='hero-light':image=linear_light(image)
        elif name=='hero-glow':image=vivid_light_gain(image)
        elif any(image.getextrema()[c]!=(0,0) for c in range(3)):raise ValueError('Vignette must be black')
        w,h=image.size;binary+=struct.pack('<II4f',w,h,*bounds)
        # Fixed atlas origin, power-of-two allocation; no hidden tex3ds crop.
        pw,ph=8,8
        while pw<w:pw*=2
        while ph<h:ph*=2
        page=Image.new('RGBA',(pw,ph));page.paste(image,(0,0))
        png=output/(name+'.png');page.save(png)
        t3x=output/(name+'.t3x')
        fmt=vignette_format(page,compact_vignette) if name=='vignette' else 'rgba8'
        tex3ds_cache.run([str(tex3ds),'-f',fmt,'-z','none','-o',str(t3x),str(png)],check=True)
        shutil.copyfile(t3x,romfs/t3x.name);sprites.append(dict(name=name,size=[w,h],bounds=bounds))
        if compact_vignette:sprites[-1]['format']=fmt
    binary+=struct.pack('<6f',*transform)
    control=read('fsm-20931')
    def action(state,kind):
        return [a['parameters'] for s in control['states'] if s['name']==state for a in s['actions'] if a.get('enabled',True) and a['type'].endswith('.'+kind)]
    scales=[]
    for state in ['Normal','Dark -1','Dark 1']:
        params=action(state,'SetVector3XYZ');assert len(params)==2
        scales.extend(p['x']['value'] for p in params)
    seconds=[action('Damage','ScaleTo')[0]['duration']['value'],action('Damage Pause','Wait')[0]['time']['value'],action('Recover','ScaleTo')[0]['duration']['value']]
    binary+=struct.pack('<9f',*scales,*seconds)
    triggers=[]
    for region,tid,ids,fid in [(1,4384,[8134,8135,8136],12051),(2,6029,[8235,8237,8238],12170)]:
        fsm=read(f'decoded-level6-{fid}');assert fsm['name']=='Darkness Region'
        darkness=next(v['value'] for v in fsm['variables']['intVariables'] if v['name']=='Darkness');assert darkness==[-1,1][region-1]
        t=read(f'level6-{tid}');p=t['m_LocalPosition'];s=t['m_LocalScale']
        for cid in ids:
            c=read(f'level6-{cid}');assert c['m_IsTrigger'] and c['m_Enabled']
            o,z=c['m_Offset'],c['m_Size']
            rect=[p['m_X']+(o['m_X']-z['m_X']/2)*s['m_X'],p['m_Y']+(o['m_Y']-z['m_Y']/2)*s['m_Y'],p['m_X']+(o['m_X']+z['m_X']/2)*s['m_X'],p['m_Y']+(o['m_Y']+z['m_Y']/2)*s['m_Y']]
            binary+=struct.pack('<4fi',*rect,region);triggers.append(dict(region=region,bounds=rect))
    for rect in borders:
        binary+=struct.pack('<4f',*rect)
    binary+=struct.pack('<6f',*glow_transform)
    light_control=read('fsm-23559')
    hero_damage=hero_damage_tween(light_control)
    idle=next(v['value']['x'] for v in light_control['variables']['vector3Variables'] if v['name']=='Idle Scale')
    assert abs(transform[0]-idle)<.0001
    binary+=struct.pack('<5f',*hero_damage)
    (output/'lighting.bin').write_bytes(binary);shutil.copyfile(output/'lighting.bin',romfs/'lighting.bin')
    report=dict(build=22529139,inputs=inputs,sprites=sprites,transform=transform,glow_transform=glow_transform,scales=scales,seconds=seconds,hero_damage=hero_damage,triggers=triggers,format='mixed' if compact_vignette else 'rgba8',bytes=len(binary))
    (output/'manifest.json').write_text(json.dumps(report,indent=2)+'\n')
    print('Lighting:',len(binary),'metadata bytes,',sum((output/(n+'.png')).stat().st_size for n in ['hero-light','vignette','hero-glow']),'PNG bytes')

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--source',type=Path,default=ROOT/'_private/research/hero-light');p.add_argument('--output',type=Path,default=ROOT/'_private/converted/hero-light');p.add_argument('--romfs',type=Path,default=ROOT/'_private/converted/hero/romfs');p.add_argument('--tex3ds',type=Path,default=Path(TEX3DS_EXE));p.add_argument('--probe',type=Path)
    p.add_argument('--original',type=Path,default=ORIGINAL_DATA)
    p.add_argument('--compact-vignette',action='store_true')
    a=p.parse_args();convert(a.source,a.output,a.romfs,a.tex3ds,a.probe,a.compact_vignette,a.original)
