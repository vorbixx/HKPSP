"""Convert the private Tutorial_01 Crawler export to HKE1 and HKA1 resources."""
import converted_files  # Install hardlink copy-on-write before asset writes.
import argparse
import hashlib
import json
import math
import shutil
import struct
import tex3ds_cache
from pathlib import Path
from private_output_paths import is_private_output
from PIL import Image
from assetripper_client import ROOT
from convert_hero import decode_sprite
from atlas_packing import pack_pages
from inspect_fsm import decode_fsm
from tool_paths import TEX3DS as TEX3DS_EXE


def observe(source):
    mapping=json.loads((source/'crawler-map.json').read_text())
    def read(name): return json.loads((source/name).read_text())
    def component(group,kind): return read(mapping[group][kind][0])
    def script(group,kind): return component(group,kind)['m_Structure']
    def require(ok,message):
        if not ok: raise ValueError(message)
    for name,expected in mapping['source_sha256'].items():
        require(hashlib.sha256((source/name).read_bytes()).hexdigest()==expected,'Changed source: '+name)
    for name in [*mapping['ancestors'],*mapping['actor']['Transform'],*mapping['corpse']['Transform'],*mapping['hero_box']['Transform']]:
        t=read(name)
        require(t['m_LocalRotation']==dict(m_W=1,m_X=0,m_Y=0,m_Z=0),'Rotated actor needs separate support')
        require(t['m_LocalScale']==dict(m_X=1,m_Y=1,m_Z=1),'Scaled actor needs separate support')
    health=script('actor','HealthManager');recoil=script('actor','Recoil');death=script('actor','EnemyDeathEffects')
    damage=script('actor','DamageHero');corpse=script('corpse','Corpse');bounce=script('corpse','ObjectBounce')
    require(not any(health[k] for k in ['invincible','damageOverride','hasSpecialDeath','hasAlternateHitAnimation','megaFlingGeo','isDead']), 'Unsupported health branch')
    require(health['enemyType']==0 and health['mediumGeoDrops']==0 and health['largeGeoDrops']==0,'Unsupported enemy/drops')
    require(not recoil['freezeInPlace'] and not recoil['preventRecoilUp'],'Unsupported recoil branch')
    require(damage['hazardType']==1 and not damage['shadowDashHazard'],'Unsupported contact damage')
    require(not any(corpse[k] for k in ['breaker','bigBreaker','massless','instantChunker','resetRotation','chunker','deathStun','fungusExplode','goopExplode','hatcher','spineBurst','zomHive']), 'Unsupported corpse branch')
    require(not any(death[k] for k in ['isCorpseRecyclable','corpseFacesRight','lowCorpseArc','recycle','rotateCorpse']) and death['doKillFreeze'], 'Unsupported death branch')
    require(not bounce['playSound'] and not bounce['playAnimationOnBounce'] and not bounce['sendFSMEvent'], 'Unsupported bounce effects')
    fsm=decode_fsm(script('actor','PlayMakerFSM')['fsm']);states={s['name']:s for s in fsm['states']}
    def action(state,kind):
        return next(a['parameters'] for a in states[state]['actions'] if a['enabled'] and a['type'].endswith('.'+kind))
    walking=action('Walk','WalkLeftRight')
    require(walking['spriteFacesLeft'] and walking['walkAnimName']['value']=='walk' and walking['turnAnimName']['value']=='turn','Unsupported animation/direction')
    require(not any(walking[k]['value'] for k in ['startLeft','startRight','keepDirection']),'Unsupported initial facing')
    first=next(v['value'] for v in fsm['variables']['boolVariables'] if v['name']=='First Crawler')
    require(not first and fsm['start']=='Walk','Tutorial gate requires its room FSM')
    require(action('Start L','Wait')['realTime'] and not action('Start R','Wait')['realTime'],'Changed facing-command timers')
    for state in ['Start L','Start R','Maintain']:
        a=action(state,'WalkLeftRight')
        for key in ['walkSpeed','turnDelay','spriteFacesLeft','groundLayer','walkAnimName','turnAnimName','gameObject']:
            require(a[key]==walking[key],'Changed walk settings between states: '+key)
        for key,expected in [('startLeft',state=='Start L'),('startRight',state=='Start R'),('keepDirection',state=='Maintain')]:
            require(not a[key]['useVariable'] and a[key]['value']==expected,'Changed start-direction branch')
    expected_transitions={'Walk':{'FIRST':'Wait','GO LEFT':'Start L','GO RIGHT':'Start R'},'Wait':{},
                          'Start L':{'FINISHED':'Maintain'},'Start R':{'FINISHED':'Maintain'},
                          'Maintain':{'GO LEFT':'Start L','GO RIGHT':'Start R'}}
    require(set(states)==set(expected_transitions),'Changed FSM states')
    for name,transitions in expected_transitions.items():
        require({t['fsmEvent']['name']:t['toState'] for t in states[name]['transitions']}==transitions,'Changed transitions')
    for state in ['Start L','Start R']:
        translate=action(state,'Translate')
        require(not any(translate[k] for k in ['perSecond','everyFrame','lateUpdate','fixedUpdate']),'Unsupported translation timing')
        require(all(translate[k]['value']==0 for k in ['y','z']) and not translate['x']['useVariable'],'Unsupported translation values')
        require(translate['vector']['useVariable'] and not translate['vector']['name'],'Unsupported translation vector')
        require(action(state,'Wait')['finishEvent']=='FINISHED','Changed timer transition')
    rects=[]
    for group in ['actor','corpse']:
        boxes=[read(name) for name in mapping[group]['BoxCollider2D']]
        first=boxes[0]
        for box in boxes:
            require(not box['m_IsTrigger'] and box['m_EdgeRadius']==0 and box['m_Enabled']==1,'Unsupported collider')
            require(box['m_Offset']==first['m_Offset'] and box['m_Size']==first['m_Size'],'Distinct colliders need separate geometry')
        o,s=first['m_Offset'],first['m_Size']
        rects.append([o['m_X']-s['m_X']/2,o['m_Y']-s['m_Y']/2,o['m_X']+s['m_X']/2,o['m_Y']+s['m_Y']/2])
        body=component(group,'Rigidbody2D')
        require(body['m_BodyType']==0 and body['m_LinearDamping']==0 and body['m_Constraints']==4,'Unsupported body configuration')
        require(body['m_Mass']==1 and not body['m_UseAutoMass'] and body['m_Simulated'] and body['m_CollisionDetection']==1, 'Unsupported body mass/simulation/continuous mode')
        require(not body['m_Material']['m_PathID'], 'Rigidbody material override needs explicit resolution')
    clips=read(mapping['library'])['m_Structure']['clips']
    require({c['name'] for c in clips}=={'walk','turn','idle','Death Air','Death Land'},'Changed clip library')
    for clip in clips:
        require(clip['wrapMode'] in (0,2) and clip['fps']>0,'Unsupported animation')
        for frame in clip['frames']:
            ref=frame['spriteCollection'];key=mapping['sprite_collections'][f'{ref["m_FileID"]}:{ref["m_PathID"]}']
            sprite=read(key)['m_Structure']['spriteDefinitions'][frame['spriteId']]
            require(sprite['colliderType']==2 and len(sprite['colliderVertices'])==2,'Unsupported sprite collider')
            o,s=sprite['colliderVertices'];rect=[o['x']-s['x'],o['y']-s['y'],o['x']+s['x'],o['y']+s['y']]
            require(rect==rects[int(clip['name'].startswith('Death'))],'Animated collider varies')
            require(not frame['triggerEvent'],'Frame callbacks require implementation')
    values=[*rects[0],*rects[1],walking['walkSpeed'],walking['turnDelay'],recoil['recoilSpeedBase'],recoil['recoilDuration'],
            component('actor','Rigidbody2D')['m_GravityScale'],component('corpse','Rigidbody2D')['m_GravityScale'],
            1,1,1,1,death['corpseSpawnPoint']['x'],death['corpseSpawnPoint']['y'],death['corpseFlingSpeed'],bounce['bounceFactor'],bounce['speedThreshold'],
            action('Start L','Wait')['time']['value'],action('Start R','Wait')['time']['value'],
            action('Start L','Translate')['x']['value'],action('Start R','Translate')['x']['value']]
    hurtbox=component('hero_box','BoxCollider2D');ht=component('hero_box','Transform')
    require(hurtbox['m_IsTrigger'] and hurtbox['m_EdgeRadius']==0 and hurtbox['m_Enabled']==1,'Unsupported HeroBox')
    require(ht['m_LocalPosition']==dict(m_X=0,m_Y=0,m_Z=0),'Offset HeroBox transform')
    require(ht['m_Father']['m_PathID']==8776 and read(mapping['hero_parent'])['m_LocalScale']==dict(m_X=1,m_Y=1,m_Z=1),'Changed Knight parent')
    o,s=hurtbox['m_Offset'],hurtbox['m_Size']
    values.extend([o['m_X']-s['m_X']/2,o['m_Y']-s['m_Y']/2,o['m_X']+s['m_X']/2,o['m_Y']+s['m_Y']/2])
    require(len(values)==31 and all(math.isfinite(v) for v in values),'Invalid parameters')
    integers=[health['hp'],damage['damageDealt'],component('actor','GameObject')['m_Layer'],len(mapping['actor']['BoxCollider2D']),health['smallGeoDrops'],0]
    return mapping,integers,values,clips


def convert(source,output,romfs,tex3ds):
    for path in [output,romfs]:
        if not converted_files.private_output(path, ROOT): raise ValueError('Resources must remain private')
        path.mkdir(parents=True,exist_ok=True)
    mapping,integers,values,clips=observe(source)
    (output/'crawler.bin').write_bytes(struct.pack('<4s6I31f',b'HKE1',*integers,*values))
    # Lossless rectangular page: retain source pixels, geometry and filter borders.
    collections={name:json.loads((source/name).read_text())['m_Structure'] for name in mapping['sprite_collections'].values()}
    if any(len(c['textures'])!=1 for c in collections.values()): raise ValueError('Multiple textures need explicit material mapping')
    atlases={name:Image.open(source/mapping['textures'][name]).convert('RGBA') for name in collections}
    def key(frame):
        r=frame['spriteCollection'];return (mapping['sprite_collections'][f'{r["m_FileID"]}:{r["m_PathID"]}'],frame['spriteId'])
    ids=sorted({key(f) for c in clips for f in c['frames']})
    sprites=[(k,*decode_sprite(atlases[k[0]],collections[k[0]]['spriteDefinitions'][k[1]])) for k in ids]
    records,pages=pack_pages(sprites,[(256,512)])
    page=pages[0]
    total=sum(len(c['frames']) for c in clips)
    with (output/'crawler-anim.bin').open('wb') as out:
        out.write(struct.pack('<4s4I',b'HKA1',1,len(ids),len(clips),total))
        for k in ids: out.write(struct.pack('<5I4f',*records[k]))
        first=0
        for c in clips:
            out.write(struct.pack('<48sf4I',c['name'].encode(),c['fps'],c['wrapMode'],c['loopStart'],first,len(c['frames'])))
            first+=len(c['frames'])
        for c in clips:
            for f in c['frames']:out.write(struct.pack('<I',ids.index(key(f))))
    page.save(output/'crawler-page-0.png')
    tex3ds_cache.run([str(tex3ds),'-f','rgba8','-z','none','-o',str(output/'crawler-page-0.t3x'),str(output/'crawler-page-0.png')],check=True)
    for name in ['crawler.bin','crawler-anim.bin','crawler-page-0.t3x']:shutil.copyfile(output/name,romfs/name)
    report=dict(source_sha256=mapping['source_sha256'],integers=integers,parameters=values,clips=clips,
                sprites=len(ids),frames=total,page_size=list(page.size),texture_format='rgba8',
                texture_bytes=page.width*page.height*4)
    (output/'manifest.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(json.dumps(dict(clips=len(clips),sprites=len(ids),frames=total,texture_bytes=report['texture_bytes'])))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,default=ROOT/'_private/research/enemy')
    parser.add_argument('--output',type=Path,default=ROOT/'_private/converted/crawler')
    parser.add_argument('--romfs',type=Path,default=ROOT/'_private/converted/hero/romfs')
    parser.add_argument('--tex3ds',type=Path,default=Path(TEX3DS_EXE))
    a=parser.parse_args();convert(a.source,a.output,a.romfs,a.tex3ds)
