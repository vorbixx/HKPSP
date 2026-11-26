"""Fungus2_04's two original Mantis levers and independently saved gates."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess

import converted_files
from PIL import Image
from assetripper_client import ROOT
from atlas_packing import pack_pages
from convert_hero import decode_sprite
from convert_room_visuals import project
from convert_world_switch_visuals import bank_bytes
from inspect_fsm import decode_fsm
from private_output_paths import is_private_output
from tool_paths import TEX3DS as TEX3DS_EXE

SOURCE = ROOT / '_private/research/crossroads-scenes/Fungus2_04/terrain'
ART = ROOT / '_private/minimum-obstacles/mantis-gate-art'
NAMES = ('Lever Idle', 'Lever Hit', 'Lever Activated', 'Door Closed', 'Door Open 1', 'Door Open 2')
PAIRS = ((143,149,3582,3595,5074,5078,'Mantis Lever'),
         (215,1157,3583,3642,5087,5170,'Mantis Lever (1)'))


def require(ok, message):
    if not ok:
        raise ValueError(message)


def source_data(source=SOURCE):
    hashes = {}
    def read(ident):
        name = f'level168-{ident}.json'
        raw = (source/name).read_bytes()
        hashes[name] = hashlib.sha256(raw).hexdigest()
        return json.loads(raw)
    def action(fsm, state, kind):
        rows = [a['parameters'] for s in fsm['states'] if s['name']==state
                for a in s['actions'] if a['enabled'] and a['type'].rsplit('.',1)[-1]==kind]
        require(len(rows)==1, 'Missing/ambiguous Mantis action: '+state+'/'+kind)
        return rows[0]
    def actor(owner):
        go = read(owner)
        require(go['m_IsActive'], 'Inactive Mantis actor')
        components = [read(c['m_Component']['m_PathID']) for c in go['m_Components']]
        transforms = [c for c in components if 'm_LocalPosition' in c]
        sprites = [c['m_Structure'] for c in components if '_spriteId' in c.get('m_Structure',{})]
        renderers = [c for c in components if 'm_SortingOrder' in c]
        require(len(transforms)==len(sprites)==len(renderers)==1, 'Ambiguous Mantis actor components')
        chain = transforms[:]
        while chain[-1]['m_Father']['m_PathID']:
            chain.append(read(chain[-1]['m_Father']['m_PathID']))
            require(len(chain)<32 and read(chain[-1]['m_GameObject']['m_PathID'])['m_IsActive'], 'Unsupported Mantis hierarchy')
        s, r = sprites[0], renderers[0]
        require(s['collection']==dict(m_FileID=3,m_PathID=58) and r['m_Enabled'], 'Changed Mantis artwork')
        origin = project((0,0,0),chain)
        x = project((s['_scale']['x'],0,0),chain)
        y = project((0,s['_scale']['y'],0),chain)
        values = [x[0]-origin[0],y[0]-origin[0],origin[0],x[1]-origin[1],y[1]-origin[1],origin[1],origin[2],
                  *[s['_color'][c] for c in 'rgba'],r['m_SortingLayer'],r['m_SortingOrder'],x[2]-origin[2],y[2]-origin[2],0]
        require(all(math.isfinite(v) for v in values), 'Nonfinite Mantis transform')
        return dict(owner=owner,sprite=s['_spriteId'],values=values,chain=chain)
    pairs = []
    times = None
    for lever,gate,trigger,solid,switch_id,gate_id,name in PAIRS:
        switch = decode_fsm(read(switch_id)['m_Structure']['fsm'])
        door = decode_fsm(read(gate_id)['m_Structure']['fsm'])
        variables = {v['name']:v['value'] for group in switch['variables'].values() if isinstance(group,list)
                     for v in group if isinstance(v,dict) and 'value' in v}
        require(read(lever)['m_Name']==name and not variables['SetPlayerData'] and
                variables['Target']==dict(m_FileID=0,m_PathID=gate), 'Changed Mantis save/target identity')
        waits = []
        for fsm,state in ((switch,'Hit'),(door,'Open Pause'),(door,'Dust')):
            wait = action(fsm,state,'Wait')
            require(not wait['realTime'] and not wait['time']['useVariable'], 'Unsupported Mantis clock')
            waits.append(wait['time']['value'])
        expected = [.75,.5,struct.unpack('<f',struct.pack('<f',.7))[0]]
        require(waits==expected and (times is None or waits==times), 'Changed Mantis waits')
        times = waits
        require(action(switch,'Open','SendEventByName')['sendEvent']['value']=='OPEN' and
                action(switch,'Activated','SendEventByName')['sendEvent']['value']=='ACTIVATE' and
                not action(door,'Open 2','SetCollider')['active']['value'] and
                not action(door,'Opened','SetMeshRenderer')['active']['value'] and
                not action(door,'Activate','SetCollider')['active']['value'] and
                not action(door,'Activate','SetMeshRenderer')['active']['value'], 'Changed Mantis gate outcomes')
        parts = [actor(lever),actor(gate)]
        c = read(trigger)
        require(c['m_Enabled'] and c['m_IsTrigger'] and c['m_GameObject']['m_PathID']==lever and
                len(c['m_Points']['m_Paths'])==1 and len(c['m_Points']['m_Paths'][0])==5, 'Changed Mantis nail polygon')
        points = [project((p['m_X']+c['m_Offset']['m_X'],p['m_Y']+c['m_Offset']['m_Y'],0),parts[0]['chain'])[:2]
                  for p in c['m_Points']['m_Paths'][0]]
        c = read(solid)
        require(c['m_Enabled'] and not c['m_IsTrigger'] and c['m_GameObject']['m_PathID']==gate, 'Changed Mantis solid')
        pairs.append(dict(parts=parts,points=points,collider=solid,name=name))
    return times,pairs,hashes


def convert(output, source=SOURCE, art=ART, tex3ds=Path(TEX3DS_EXE)):
    require(is_private_output(output), 'Original Mantis artwork must remain private')
    times,pairs,hashes = source_data(source)
    manifest = json.loads((art/'sources.json').read_text())
    inputs = [manifest['parts'][k]['file'] for k in ('library','collection')]+manifest['textures']
    art_hashes = {n:hashlib.sha256((art/n).read_bytes()).hexdigest() for n in inputs}
    for name,expected in manifest.get('sha256',{}).items():
        require(hashlib.sha256((art/name).read_bytes()).hexdigest()==expected, 'Changed original Mantis input: '+name)
    for name,expected in manifest['scene_sources_sha256'].items():
        require(hashlib.sha256((source/name).read_bytes()).hexdigest()==expected, 'Changed Mantis scene provenance')
    library = json.loads((art/inputs[0]).read_text())['m_Structure']
    collection = json.loads((art/inputs[1]).read_text())['m_Structure']
    clips = []
    for name in NAMES:
        candidates = [c for c in library['clips'] if c['name']==name]
        require(len(candidates)==1, 'Missing/duplicate Mantis clip: '+name)
        c = candidates[0]
        require(math.isfinite(c['fps']) and c['fps']>0 and c['frames'] and c['wrapMode'] in (0,2,6), 'Unsupported Mantis clip')
        require(all(f['spriteCollection']==dict(m_FileID=0,m_PathID=58) and not f.get('triggerEvent') for f in c['frames']), 'Unsupported Mantis animation event/reference')
        clips.append(c)
    require(all(clips[i]['wrapMode']==2 for i in (1,4,5)), 'Mantis completion must remain Once')
    ids = sorted({f['spriteId'] for c in clips for f in c['frames']} |
                 {p['sprite'] for pair in pairs for p in pair['parts']})
    require(len(ids)<=64, 'Mantis sprite admission exceeds fixed budget')
    texture = Image.open(art/manifest['textures'][0]).convert('RGBA')
    sprites = [(sid,*decode_sprite(texture,collection['spriteDefinitions'][sid])) for sid in ids]
    records,pages = pack_pages(sprites,[(1024,1024)])
    require(len(pages)==1, 'Mantis artwork exceeds one-page budget')
    output.mkdir(parents=True,exist_ok=True)
    png = output/'mantis-gate-page-0.png'
    pages[0].save(png)
    subprocess.run([str(tex3ds),'-f','rgba4','-z','none','-o',str(png.with_suffix('.t3x')),str(png)],check=True)
    (output/'mantis-gate.bin').write_bytes(bank_bytes(clips,ids,records,1))
    raw = struct.pack('<4s3f',b'MTG1',*times)
    for pair in pairs:
        raw += struct.pack('<I',pair['collider'])
        for p in pair['parts']:
            raw += struct.pack('<II16f',p['owner'],ids.index(p['sprite']),*p['values'])
        raw += struct.pack('<10f',*(v for point in pair['points'] for v in point))
    (output/'mantis-gate-room.bin').write_bytes(raw)
    report = dict(scene='Fungus2_04',times=times,pairs=[dict(name=p['name'],collider=p['collider'],points=p['points']) for p in pairs],
                  pages=1,sprites=len(ids),source_sha256=hashes,art_sha256=art_hashes,
                  remaining='Original gate dust and switch contact/audio feedback need runtime binding')
    (output/'mantis-gate-manifest.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:report[k] for k in ('scene','times','pages','sprites')}))


if __name__=='__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path,required=True)
    args = p.parse_args()
    convert(args.output)
