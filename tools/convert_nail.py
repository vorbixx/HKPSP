"""Pack privately exported nail polygons and runtime transforms; no game data here."""
import converted_files  # Install hardlink copy-on-write before asset writes.
import hashlib
import json
import math
import struct
from pathlib import Path


def observations(assets):
    used = {}
    def read(name):
        raw = (assets/name).read_bytes()
        used[name] = hashlib.sha256(raw).hexdigest()
        return json.loads(raw)
    def asset(i):
        return read(f'c1-{i}.json')
    records = []
    matrix = read('Physics2DSettings.json')['m_LayerCollisionMatrix']
    for entry in read('combat-map.json'):
        slash, transform, collider = (asset(entry[k]) for k in ['script','transform','collider'])
        # All five measured attack ancestors are identity transforms. Reject
        # another build instead of silently dropping rotations or parent scale.
        for i in [entry['transform'], *entry['ancestors']]:
            t = asset(i)
            if t['m_LocalRotation'] != {'m_W':1,'m_X':0,'m_Y':0,'m_Z':0}:
                raise ValueError('Unsupported rotated attack transform')
            if i != entry['transform']:
                if t['m_LocalScale']['m_X'] != 1 or t['m_LocalScale']['m_Y'] != 1:
                    raise ValueError('Unexpected attack ancestor scale')
                if t['m_Father']['m_PathID'] and any(t['m_LocalPosition'][k] for k in ['m_X','m_Y']):
                    raise ValueError('Unexpected attack ancestor offset')
        if not collider['m_IsTrigger'] or len(collider['m_Points']['m_Paths']) != 1:
            raise ValueError('Expected one trigger polygon')
        points = [(p['m_X']+collider['m_Offset']['m_X'],p['m_Y']+collider['m_Offset']['m_Y'])
                  for p in collider['m_Points']['m_Paths'][0]]
        if not 3 <= len(points) <= 16:
            raise ValueError('Unsupported polygon size')
        crosses = []
        for i,p in enumerate(points):
            q,r=points[(i+1)%len(points)],points[(i+2)%len(points)]
            crosses.append((q[0]-p[0])*(r[1]-q[1])-(q[1]-p[1])*(r[0]-q[0]))
        if not (all(c>0 for c in crosses) or all(c<0 for c in crosses)):
            raise ValueError('Non-convex source polygon requires decomposition')
        layer = asset(entry['game_object'])['m_Layer']
        # StartSlash overrides the serialized Transform scale with NailSlash.scale.
        s = slash['m_Structure']
        position = [transform['m_LocalPosition'][k] for k in ['m_X','m_Y']]
        scale = [s['scale'][k] for k in ['x','y']]
        if not all(math.isfinite(v) for v in [*position,*scale,*sum((list(p) for p in points),[])]):
            raise ValueError('Non-finite geometry')
        if any(v == 0 for v in scale):
            raise ValueError('Zero attack scale')
        records.append(dict(kind=entry['kind'],position=position,scale=scale,
                            clip=s['animName'],layer_mask=matrix[layer],points=points))
    if [r['kind'] for r in records] != ['normalSlash','alternateSlash','upSlash','downSlash','wallSlash']:
        raise ValueError('Unexpected attack order')
    return records,used


def convert(assets, output):
    records, hashes = observations(assets)
    with (output/'nail.bin').open('wb') as target:
        target.write(struct.pack('<4sI',b'HKN1',len(records)))
        for r in records:
            name=r['clip'].encode()
            if len(name)>=48: raise ValueError('Clip name too long')
            target.write(struct.pack('<4fII48s',*r['position'],*r['scale'],r['layer_mask'],len(r['points']),name))
            for p in r['points']: target.write(struct.pack('<2f',*p))
    (output/'nail-manifest.json').write_text(json.dumps({'format':'HKN1','source_sha256':hashes,'attacks':records},indent=2))
