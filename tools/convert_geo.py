"""Derive small-geo gameplay configuration from private prefab and source observations."""
import converted_files  # Install hardlink copy-on-write before asset writes.
import argparse
import hashlib
import json
import re
import struct
from pathlib import Path
from private_output_paths import is_private_output
ROOT=Path(__file__).resolve().parents[1]


def method_source(directory, name):
    """Accept both saved ILSpy naming conventions without changing source bytes."""
    for filename in (name, Path(name).stem + '.decompiled.cs'):
        path = directory / filename
        if path.is_file():
            return path
    raise FileNotFoundError('Missing audited geo source: ' + str(directory / name))


def depth_range(source):
    found = re.search(r'SetPositionZ\((?:UnityEngine\.)?Random\.Range\((0\.001)f,\s*(0\.002)f\)\)', source)
    if not found:
        raise ValueError('Changed audited geo depth branch')
    return [float(value) for value in found.groups()]


def convert(source,output):
    # Shared research may be a worktree junction; keep checkout-relative provenance.
    source=source.absolute();output=output.absolute()
    if not converted_files.private_output(output, ROOT):raise ValueError('Geo records must remain private')
    m=json.loads((source/'sources.json').read_text());hashes={}
    def read(name):
        path=source/name;raw=path.read_bytes();sha=hashlib.sha256(raw).hexdigest()
        if sha!=m['sha256'][name]:raise ValueError('Changed geo prefab source '+name)
        hashes[str(path.relative_to(ROOT))]=sha;return json.loads(raw)
    def part(kind):return read(m['parts'][kind][0])
    body=part('Rigidbody2D');box=part('BoxCollider2D');t=part('Transform');control=part('GeoControl')['m_Structure'];bounce=part('ObjectBounce')['m_Structure'];mat=read(m['material'])
    if body['m_LinearDamping']!=0 or body['m_Constraints']!=4 or box['m_IsTrigger'] or control['type']!=0:raise ValueError('Unsupported small-geo prefab branch')
    if bounce['playAnimationOnBounce'] or bounce['sendFSMEvent']:raise ValueError('Unexpected bounce callback branch')
    auto=part('AutoRecycleSelf')['m_Structure']
    if auto['afterEvent']!=2:raise ValueError('Geo lifetime no longer level unload')
    def method(name):
        path=method_source(ROOT/'_private/research/decompiled/all',name);raw=path.read_bytes();hashes[str(path.relative_to(ROOT))]=hashlib.sha256(raw).hexdigest();return raw.decode('utf-8-sig')
    health=method('HealthManager.cs');geo=method('GeoControl.cs');fling=method('FlingUtils.cs');ob=method('ObjectBounce.cs');method('EnemyDeathEffects.cs')
    def match(pattern,text):
        found=re.search(pattern,text)
        if not found:raise ValueError('Changed audited source branch: '+pattern)
        return float(found[1])
    speeds=[match(r'float speed'+k+r' = \(megaFlingGeo \? \d+ : (\d+)\);',health) for k in ('Min','Max')]
    angles=[match(r'float angle'+k+r' = \(megaFlingGeo \? \d+ : (\d+)\);',health) for k in ('Min','Max')]
    delay=match(r'pickupStartDelay = ([0-9.]+)f;',geo)
    bmin=match(r'Random.Range\(([0-9.]+)f, 1.2f\)',ob);bmax=match(r'Random.Range\(0.8f, ([0-9.]+)f\)',ob)
    zmin,zmax=depth_range(geo)
    settings=ROOT/'_private/research/assets/Physics2DSettings.json';raw=settings.read_bytes();hashes[str(settings.relative_to(ROOT))]=hashlib.sha256(raw).hexdigest();gravity=json.loads(raw)['m_Gravity']['m_Y']*body['m_GravityScale']
    sx=t['m_LocalScale']['m_X'];sy=t['m_LocalScale']['m_Y'];ox=box['m_Offset']['m_X'];oy=box['m_Offset']['m_Y'];w=box['m_Size']['m_X'];h=box['m_Size']['m_Y']
    bounds=[(ox-w/2)*sx,(oy-h/2)*sy,(ox+w/2)*sx,(oy+h/2)*sy]
    enemies=[]
    def checked(path,sha):
        raw=path.read_bytes();actual=hashlib.sha256(raw).hexdigest()
        if actual!=sha:raise ValueError('Changed enemy export '+str(path))
        hashes[str(path.relative_to(ROOT))]=actual;return json.loads(raw)['m_Structure']
    crawler=ROOT/'_private/research/enemy';cm=json.loads((crawler/'crawler-map.json').read_text())
    ck=cm['actor']['HealthManager'][0];cd=cm['actor']['EnemyDeathEffects'][0]
    ch=checked(crawler/ck,cm['source_sha256'][ck]);ce=checked(crawler/cd,cm['source_sha256'][cd]);enemies.append((ch,ce))
    tutorial=ROOT/'_private/research/tutorial';inv=json.loads((tutorial/'inventory.json').read_text())
    row=next(r for r in inv['rows'] if r['collider']=='level6-8285.json');bk=row['parts']['HealthManager'][0];bd=row['parts']['EnemyDeathEffects'][0]
    bh=checked(tutorial/bk,inv['source_sha256'][bk]);be=checked(tutorial/bd,inv['source_sha256'][bd]);enemies.append((bh,be))
    if any(h['megaFlingGeo'] or h['mediumGeoDrops'] or h['largeGeoDrops'] for h,_ in enemies):raise ValueError('Only ordinary small-geo Tutorial branch supported')
    values=bounds+[mat['m_Friction'],mat['m_Bounciness'],gravity,delay]+speeds+angles+[bounce['bounceFactor'],bounce['speedThreshold'],bmin,bmax,zmin,zmax]
    data=bytearray(struct.pack('<4s18fI',b'GEO1',*values,control['sizes'][0]['value']))
    for h,e in enemies:
        p=h['effectOrigin'];key=e['playerDataName'].encode()
        if not key or len(key)>=48:raise ValueError('Invalid journal key')
        data+=struct.pack('<2f48s',p['x'],p['y'],key)
    output.mkdir(parents=True,exist_ok=True);(output/'geo.bin').write_bytes(data)
    report=dict(build_id=22529139,format='GEO1',body=bounds,gravity=gravity,pickup_delay=delay,values=values,enemy_keys=[e['playerDataName'] for _,e in enemies],source_sha256=hashes,sha256=hashlib.sha256(data).hexdigest(),limits=['No Gathering Swarm/Greed charms enabled in Tutorial entry branch','Terrain solver does not model dynamic geo/geo or hero-solid collision','ObjectBounce trajectory follows source sampling/action semantics; no paced source trajectory comparison','Unity RNG sequence/audio random draws are not replicated','OnTriggerEnter pickup does not automatically repeat after initial delay'])
    (output/'geo-manifest.json').write_text(json.dumps(report,indent=2));return report


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',type=Path,default=ROOT/'_private/research/geo');p.add_argument('--output',type=Path,default=ROOT/'_private/converted/tutorial')
    a=p.parse_args();r=convert(a.source,a.output);print(json.dumps({k:r[k] for k in ('gravity','pickup_delay','sha256')}))
