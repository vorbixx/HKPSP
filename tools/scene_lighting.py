"""Source-validated room lighting, grading and reviewed darkness regions."""
import hashlib
import json
import math
import struct
from functools import lru_cache
from pathlib import Path

SCENE_LIGHT = Path(__file__).resolve().parents[1] / '_private/research/hero-light/level6-12820.json'
SCENE_LIGHT_SHA256 = '340348fd66f6c7f2ce062ecd28fef57ff2d9dc1abb37ebf3a30d61c0c7c57151'
DARKNESS_TRIGGER_CAPACITY = 16  # LightingConfig::triggerCapacity; eight vertices per part.


def scene_ambient(path=SCENE_LIGHT):
    raw = path.read_bytes()
    if hashlib.sha256(raw).hexdigest() != SCENE_LIGHT_SHA256:
        raise ValueError('Changed Tutorial ambient light source')
    scene = json.loads(raw)['m_Structure']
    # SceneManager.SetLighting: Lerp(1, intensity, AmbientIntesityMix=.5).
    intensity = 1+(scene['defaultIntensity']-1)*.5
    return [scene['defaultColor'][c]*intensity for c in 'rgb']


def curve_value(keys, x):
    """Unity's unweighted Hermite curve, from the saved SceneManager."""
    if x <= keys[0]['time']:return keys[0]['value']
    for a,b in zip(keys,keys[1:]):
        if x > b['time']:continue
        dt=b['time']-a['time'];t=(x-a['time'])/dt
        return ((2*t**3-3*t*t+1)*a['value']+(t**3-2*t*t+t)*dt*a['outSlope']+
                (-2*t**3+3*t*t)*b['value']+(t**3-t*t)*dt*b['inSlope'])
    return keys[-1]['value']


def tev_curve(constants, value):
    a,b,c=constants;x=value/255
    byte=lambda v:max(0,min(255,math.floor(v+.5)))
    v=byte(2*(b*x+a*(1-x)))
    v=byte(c*x+v*(1-x))
    v=byte(v*(1-x)+128*x)
    return byte(2*v*x)


@lru_cache(maxsize=32)
def fit_curve(serialized):
    """Fit source curves while retaining shadow detail in the bounded TEV pass."""
    keys=json.loads(serialized)
    if any(k.get('weightedMode',0) for k in keys):raise ValueError('Weighted scene curve')
    expected=[max(0,min(255,curve_value(keys,x/255)*255)) for x in range(256)]
    # Relative luminance error protects dark scenery from a fit dominated by
    # bright samples. The floor keeps zero/near-zero values finite; this changes
    # only offline coefficients, never authored ambient or the runtime pass.
    def error(values):return sum((tev_curve(values,x)-v)**2/(16+v)
                                 for x,v in enumerate(expected))
    best=[64,64,128];score=error(best)
    for step in (64,32,16,8,4,2,1):
        for _ in range(16):
            improved=False
            for channel in range(3):
                for direction in (-1,1):
                    candidate=best.copy();candidate[channel]=max(0,min(255,candidate[channel]+direction*step))
                    cost=error(candidate)
                    if cost<score:best,score=candidate,cost;improved=True
            if not improved:break
    # Byte rounding makes this non-convex. Approach from black as well,
    # choosing the best axis move; retain the original fit if it is better.
    alternate=[0,0,0];alternate_score=error(alternate)
    for step in (64,32,16,8,4,2,1):
        for _ in range(16):
            candidates=[]
            for channel in range(3):
                for direction in (-1,1):
                    candidate=alternate.copy();candidate[channel]=max(0,min(255,candidate[channel]+direction*step))
                    candidates.append((error(candidate),candidate))
            cost,candidate=min(candidates)
            if cost>=alternate_score:break
            alternate,alternate_score=candidate,cost
    if alternate_score<score:best,score=alternate,alternate_score
    return tuple(best),max(abs(tev_curve(best,x)-v) for x,v in enumerate(expected))


@lru_cache(maxsize=32)
def fit_curve_pair(serialized):
    """Fit two existing TEV curves when one cannot follow the authored knots.

    Approximation: composition of two bounded polynomials, rather than the
    original dependent LUT read unavailable on PICA. Reuse the ping-pong targets.
    """
    import numpy as np
    keys=json.loads(serialized)
    single,single_error=fit_curve(serialized)
    if single_error<=4:
        return None,single,single_error
    values=np.arange(256)
    expected=np.array([max(0,min(255,curve_value(keys,x/255)*255)) for x in values])
    byte=lambda v:np.clip(np.floor(v+.5),0,255)
    def curve(p,values):
        a,b,c=p;x=values/255
        v=byte(2*(b*x+a*(1-x)))
        v=byte(c*x+v*(1-x))
        v=byte(v*(1-x)+128*x)
        return byte(2*v*x)
    def output(p):return curve(p[3:],curve(p[:3],values))
    def error(p):return float(np.sum((output(p)-expected)**2))
    identity=[64,64,128]
    rng=np.random.default_rng(184)
    seeds=[list(single)+identity,identity+list(single)]
    seeds.extend(rng.integers(0,256,6).tolist() for _ in range(16))
    best=None;score=float('inf')
    for seed in seeds:
        p=seed[:];cost=error(p)
        for step in (64,32,16,8,4,2,1):
            for _ in range(14):
                changed=False
                for i in range(6):
                    for direction in (-1,1):
                        q=p.copy();q[i]=max(0,min(255,q[i]+direction*step))
                        candidate=error(q)
                        if candidate<cost:p,cost,changed=q,candidate,True
                if not changed:break
        if cost<score:best,score=p,cost
    maximum=float(np.max(np.abs(output(best)-expected)))
    single_score=sum((tev_curve(single,x)-v)**2 for x,v in enumerate(expected))
    if maximum>=single_error or score>=single_score:
        return None,single,single_error
    return tuple(best[:3]),tuple(best[3:]),maximum


def scene_saturation(scene):
    """SceneManager.AdjustSaturation for the original desktop presentation."""
    # SceneManager.cs:335-347 adds saturationOffset (.17), without clamping.
    return scene['saturation'] + (0 if scene.get('ignorePlatformSaturationModifiers', False) else .17)


def darkness_polygon_parts(vertices):
    """Partition a simple region exactly into the existing bounded convex triggers."""
    polygon = [tuple(v) for v in vertices]
    if not 3 <= len(polygon) <= 64 or not all(math.isfinite(v) for p in polygon for v in p):
        raise ValueError('Invalid Darkness Region polygon')
    if len(set(polygon)) != len(polygon):
        raise ValueError('Repeated Darkness Region polygon vertex')

    def cross(a, b, c):
        value = (b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0])
        if not math.isfinite(value):
            raise ValueError('Invalid Darkness Region polygon')
        return value

    def on_segment(a, b, p):
        return (cross(a, b, p) == 0 and min(a[0], b[0]) <= p[0] <= max(a[0], b[0])
                and min(a[1], b[1]) <= p[1] <= max(a[1], b[1]))

    edges = list(zip(polygon, polygon[1:]+polygon[:1]))
    for i, (a, b) in enumerate(edges):
        for j in range(i+1, len(edges)):
            if j == i+1 or (i == 0 and j == len(edges)-1):
                continue
            c, d = edges[j]
            if (on_segment(a, b, c) or on_segment(a, b, d) or on_segment(c, d, a)
                    or on_segment(c, d, b) or
                    ((cross(a, b, c) > 0) != (cross(a, b, d) > 0)
                     and (cross(c, d, a) > 0) != (cross(c, d, b) > 0))):
                raise ValueError('Expected simple Darkness Region polygon')
    area = sum(a[0]*b[1]-a[1]*b[0] for a, b in edges)
    if not math.isfinite(area) or area == 0:
        raise ValueError('Degenerate Darkness Region polygon')
    if area < 0:
        polygon.reverse()

    def convex(points):
        for a, b in zip(points, points[1:]+points[:1]):
            sides = [cross(a, b, p) for p in points]
            if min(sides) < 0 or max(sides) <= 0:
                return False
        return True

    if len(polygon) <= 8 and convex(polygon):
        return [[list(p) for p in polygon]]
    remaining = polygon[:]
    parts = []
    while len(remaining) > 3:
        for i, b in enumerate(remaining):
            a, c = remaining[i-1], remaining[(i+1) % len(remaining)]
            turn = cross(a, b, c)
            if turn == 0 and on_segment(a, c, b):
                remaining.pop(i)
                break
            if turn <= 0:
                continue
            if any(cross(a, b, p) >= 0 and cross(b, c, p) >= 0 and cross(c, a, p) >= 0
                   for j, p in enumerate(remaining) if j not in ((i-1) % len(remaining), i, (i+1) % len(remaining))):
                continue
            parts.append([a, b, c])
            remaining.pop(i)
            break
        else:
            raise ValueError('Cannot partition Darkness Region polygon')
    if len(remaining) == 3 and cross(*remaining) > 0:
        parts.append(remaining)
    changed = True
    while changed:
        changed = False
        for i, left in enumerate(parts):
            for j in range(i+1, len(parts)):
                right = parts[j]
                for a in range(len(left)):
                    for b in range(len(right)):
                        if left[a] != right[(b+1) % len(right)] or left[(a+1) % len(left)] != right[b]:
                            continue
                        merged = ([left[(a+1+k) % len(left)] for k in range(len(left))] +
                                  [right[(b+2+k) % len(right)] for k in range(len(right)-2)])
                        if len(merged) <= 8 and convex(merged):
                            parts[i] = merged
                            parts.pop(j)
                            changed = True
                            break
                    if changed: break
                if changed: break
            if changed: break
    return [[list(p) for p in part] for part in parts]


def _legacy_darkness_regions(source):
    """Root box/polygon regions, including authored in-plane rotation."""
    from convert_terrain import transform_point
    terrain = source/'terrain'
    regions = []
    for path in sorted(terrain.glob('*.json')):
        component = json.loads(path.read_text())
        fsm = component.get('m_Structure', {}).get('fsm', {})
        if fsm.get('name') != 'Darkness Region' or not component.get('m_Enabled', 1):
            continue
        prefix = path.stem.rsplit('-', 1)[0]
        read = lambda ident: json.loads((terrain/f'{prefix}-{ident}.json').read_text())
        owner = read(component['m_GameObject']['m_PathID'])
        if not owner['m_IsActive']:
            continue
        level = next(v['value'] for v in fsm['variables']['intVariables'] if v['name'] == 'Darkness')
        if level not in (-1, 0, 1):
            raise ValueError('Unsupported Darkness Region level')
        parts = [read(c['m_Component']['m_PathID']) for c in owner['m_Components']]
        transforms = [c for c in parts if 'm_LocalPosition' in c]
        if len(transforms) != 1:
            raise ValueError('Expected Darkness Region transform')
        transform = transforms[0]; p = transform['m_LocalPosition']; s = transform['m_LocalScale']
        q=transform['m_LocalRotation']
        if (transform['m_Father']['m_PathID'] or q['m_X'] or q['m_Y'] or
                s['m_X'] <= 0 or s['m_Y'] <= 0):
            raise ValueError('Unsupported non-root/out-of-plane Darkness Region')
        transform_point((0,0),[transform])  # Validate the authored quaternion.
        colliders = [c for c in parts if c.get('m_IsTrigger') and c.get('m_Enabled', 1)]
        if not colliders:
            raise ValueError('Expected Darkness Region collider')
        for collider in colliders:
            o=collider['m_Offset']
            if 'm_Size' in collider:
                size=collider['m_Size']
                paths=[None]
                if q['m_Z']:
                    paths=[[dict(m_X=x,m_Y=y) for x,y in
                            ((-size['m_X']/2,-size['m_Y']/2),(size['m_X']/2,-size['m_Y']/2),
                             (size['m_X']/2,size['m_Y']/2),(-size['m_X']/2,size['m_Y']/2))]]
                box=[p['m_X']+(o['m_X']-size['m_X']/2)*s['m_X'],
                     p['m_Y']+(o['m_Y']-size['m_Y']/2)*s['m_Y'],
                     p['m_X']+(o['m_X']+size['m_X']/2)*s['m_X'],
                     p['m_Y']+(o['m_Y']+size['m_Y']/2)*s['m_Y']]
            elif 'm_Points' in collider:
                paths=collider['m_Points']['m_Paths']
                if len(paths)!=1:raise ValueError('Expected single-path Darkness Region polygon')
            else:raise ValueError('Unsupported Darkness Region collider')
            for points in paths:
                parts=[[]]
                if points is not None:
                    vertices=[transform_point((o['m_X']+v['m_X'],o['m_Y']+v['m_Y']),[transform])
                              for v in points]
                    parts=darkness_polygon_parts(vertices)
                for vertices in parts:
                    bounds=([min(v[0] for v in vertices),min(v[1] for v in vertices),
                             max(v[0] for v in vertices),max(v[1] for v in vertices)] if vertices else box)
                    if (not all(math.isfinite(v) for v in bounds) or
                            bounds[0] >= bounds[2] or bounds[1] >= bounds[3]):
                        raise ValueError('Invalid Darkness Region bounds')
                    region=dict(bounds=bounds, region={-1:1,1:2,0:3}[level], source=path.name)
                    if vertices:region['vertices']=vertices
                    regions.append(region)
    if len(regions) > DARKNESS_TRIGGER_CAPACITY:
        raise ValueError('Darkness regions exceed the fixed runtime table')
    return regions



def _convex_darkness_parts(points):
    """Triangulate a simple counterclockwise contour for the fixed SAT table."""
    cross=lambda a,b,c:(b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0])
    if len(points)<=8 and all(cross(a,b,p)>=0 for a,b in zip(points,points[1:]+points[:1]) for p in points):
        return [points]
    pending=list(range(len(points)));parts=[]
    while len(pending)>3:
        for i,index in enumerate(pending):
            previous,following=pending[i-1],pending[(i+1)%len(pending)]
            triangle=[points[previous],points[index],points[following]]
            if cross(*triangle)<=0:continue
            if any(all(cross(a,b,points[j])>=0 for a,b in zip(triangle,triangle[1:]+triangle[:1]))
                   for j in pending if j not in (previous,index,following)):
                continue
            parts.append(triangle);pending.pop(i);break
        else:raise ValueError('Invalid Darkness Region contour')
    triangle=[points[i] for i in pending]
    if cross(*triangle)<=0:raise ValueError('Invalid Darkness Region contour')
    parts.append(triangle)
    # Merge adjacent triangles into bounded convex polygons, conserving slots.
    def merge(first,second):
        for i,a in enumerate(first):
            b=first[(i+1)%len(first)]
            for j,c in enumerate(second):
                if c!=b or second[(j+1)%len(second)]!=a:continue
                contour=first[i+1:]+first[:i+1]+(second[j+1:]+second[:j+1])[1:-1]
                if len(contour)<=8 and all(cross(x,y,p)>=0 for x,y in zip(contour,contour[1:]+contour[:1]) for p in contour):
                    return contour
        return None
    while True:
        pair=next(((i,j,merged) for i in range(len(parts)) for j in range(i+1,len(parts))
                   if (merged:=merge(parts[i],parts[j])) is not None),None)
        if pair is None:return parts
        i,j,merged=pair;parts[i]=merged;parts.pop(j)


def darkness_regions(source):
    """Recover active Darkness Region triggers and their authored FSM level."""
    from convert_terrain import paths, transform_point
    terrain = source/'terrain'
    index = terrain/'terrain-map.json'
    if not index.is_file():
        # Older component-only exports predate terrain-map.json.
        regions=_legacy_darkness_regions(source)
        for region in regions:
            region['darkness']={1:-1,2:1,3:0}[region.pop('region')]
        return regions
    mapping = json.loads(index.read_text())
    read = lambda name: json.loads((terrain/name).read_text())
    regions = []
    for row in mapping['inventory']:
        go = read(row['go'])
        if go.get('m_Name') != 'Darkness Region' or not go.get('m_IsActive', True):
            continue
        collider = read(row['collider'])
        if not collider.get('m_Enabled', True) or not collider.get('m_IsTrigger'):
            continue
        fsms = row['components'][0].get('PlayMakerFSM', [])
        levels = [v['value'] for name in fsms
                  for v in read(name)['m_Structure']['fsm']['variables']['intVariables']
                  if v['name'] == 'Darkness']
        if len(levels) != 1 or levels[0] not in (-1, 0, 1, 2):
            raise ValueError('Unsupported Darkness Region FSM')
        chain = [read(name) for name in row['chain']]
        contours = [[transform_point(p, chain) for p in contour] for contour in paths(collider, row['type'])]
        points = [p for contour in contours for p in contour]
        bounds = [min(p[0] for p in points), min(p[1] for p in points),
                  max(p[0] for p in points), max(p[1] for p in points)]
        if any(not math.isfinite(v) for v in bounds) or bounds[0]>=bounds[2] or bounds[1]>=bounds[3]:
            raise ValueError('Invalid Darkness Region bounds')
        region=dict(bounds=bounds, darkness=levels[0])
        # Axis-aligned boxes stay compact. Other contours use the shared SAT path.
        corners={(bounds[0],bounds[1]),(bounds[0],bounds[3]),(bounds[2],bounds[1]),(bounds[2],bounds[3])}
        if len(contours)!=1 or len(points)!=4 or set(map(tuple,points))!=corners:
            if len(contours)!=1 or not 3<=len(points)<=64:
                raise ValueError('Unsupported Darkness Region contour')
            area=sum(a[0]*b[1]-b[0]*a[1] for a,b in zip(points,points[1:]+points[:1]))
            if area<0:points.reverse()
            for part in darkness_polygon_parts(points):
                part_bounds=[min(p[0] for p in part),min(p[1] for p in part),
                             max(p[0] for p in part),max(p[1] for p in part)]
                regions.append(dict(bounds=part_bounds,darkness=levels[0],vertices=part))
            continue
        regions.append(region)
    if len(regions)>DARKNESS_TRIGGER_CAPACITY:
        raise ValueError('Darkness regions exceed the fixed runtime table')
    return regions


def export_scene_lighting(source, target):
    """Export this room's light and post color, checking its baked ambient."""
    path=source/'scene-manager.json';raw=path.read_bytes();scene=json.loads(raw)
    ambient=[scene['defaultColor'][c]*(1+(scene['defaultIntensity']-1)*.5) for c in 'rgb']
    mapping=json.loads((source/'visuals/room-visuals-map.json').read_text())
    if mapping.get('scene_manager_sha256')!=hashlib.sha256(raw).hexdigest() or mapping.get('scene_ambient_rgb')!=ambient:
        raise ValueError('Room artwork ambient does not match its SceneManager')
    hero=scene['heroLightColor'];darkness=scene['darknessLevel'];saturation=scene_saturation(scene)
    if darkness not in (-1,0,1,2) or not math.isfinite(saturation) or saturation<0 or not 0<=hero['a']<=1:
        raise ValueError('Unsupported scene lighting range')
    hero_rgb=[hero[c] for c in 'rgb']
    if any(not math.isfinite(c) or c<0 for c in hero_rgb):raise ValueError('Invalid HeroLight RGB')
    serialized=[json.dumps(scene[c+'Channel']['m_Curve'],sort_keys=True) for c in ('red','green','blue')]
    fits=[fit_curve_pair(keys) for keys in serialized]
    two_pass=any(f[0] is not None for f in fits)
    if two_pass:
        # Every channel traverses both passes; include identity-pass rounding
        # when reporting the error of channels that keep the single-pass fit.
        pre=[f[0] or (64,64,128) for f in fits]
        preliminary=[0xff000000+sum(pre[c][i]<<(c*8) for c in range(3)) for i in range(3)]
    packed=[0xff000000+sum(fits[c][1][i]<<(c*8) for c in range(3)) for i in range(3)]
    # The existing one-pass TEV interpolation cannot extrapolate above one.
    # Keep that approximation explicit instead of overflowing the packed bytes.
    # The brightness acceptance rooms (.92/.87/.87) do not hit this bound.
    encoded_saturation=min(saturation,1.)
    packed.append(0xff000000+int(encoded_saturation*255+.5)*0x010101)
    grade_path=source/'scene-grade.json'
    grade_info=None
    payload=struct.pack('<4sif4I3f',b'HLS2',darkness,hero['a'],*packed,*hero_rgb)
    if grade_path.exists():
        grade_raw=grade_path.read_bytes();grade=json.loads(grade_raw)
        region=grade['position']+[grade['maxIntensityRadius'],grade['cutoffRadius']]
        base=[scene['defaultColor'][c] for c in 'rgb']+[scene['defaultIntensity']]
        destination=[grade['ambientColor'][c] for c in 'rgb']+[grade['ambientIntensity']]
        light=[grade['heroLightColor'][c] for c in 'rgba']
        if (len(region)!=5 or not all(math.isfinite(v) for v in region+base+destination+light) or
                not 0<=region[3]<region[4] or not 0<=light[3]<=1 or
                any(v<=0 for v in base[:3]) or any(v<0 for v in destination+light+base)):
            raise ValueError('Invalid radial GradeMarker')
        grade_fits=[fit_curve(json.dumps(grade[c+'Channel']['m_Curve'],sort_keys=True)) for c in ('red','green','blue')]
        grade_packed=[0xff000000+sum(grade_fits[c][0][i]<<(c*8) for c in range(3)) for i in range(3)]
        grade_sat=grade['saturation']+.17
        if not math.isfinite(grade_sat) or grade_sat<0:raise ValueError('Invalid grade saturation')
        grade_packed.append(0xff000000+int(min(grade_sat,1.)*255+.5)*0x010101)
        materials={}
        owners=set()
        for row in mapping['inventory']:
            for material in row['materials']:
                if material not in materials:
                    materials[material]=json.loads((source/'visuals'/material).read_text())['m_Name']
                if materials[material]=='Crossroads_Material':
                    if row.get('_owner_namespace') is not None:raise ValueError('Unsupported additive grade owner')
                    owners.add(int(row['go'].rsplit('-',1)[1].split('.')[0]))
        owners=sorted(owners)
        payload=b'HLS3'+payload[4:]+struct.pack('<17f4II',*(region+base+destination+light),*grade_packed,len(owners))
        payload+=struct.pack('<'+str(len(owners))+'I',*owners)
        grade_info=dict(source_sha256=hashlib.sha256(grade_raw).hexdigest(),region=region,
                        ambient=destination,hero=light,lit_owners=len(owners),curve_max_error=[f[1] for f in grade_fits])
    regions=darkness_regions(source)
    if regions or two_pass:
        # HLS7: independent grade/prepass flags, authored levels and convex contours.
        flags=int(grade_info is not None)|(2 if two_pass else 0)
        payload=b'HLS7'+payload[4:40]+struct.pack('<I',flags)+payload[40:]
        payload+=struct.pack('<I',len(regions))
        for r in regions:
            payload+=struct.pack('<4fi',*r['bounds'],r['darkness'])
            vertices=r.get('vertices',[])
            payload+=struct.pack('<I',len(vertices))+b''.join(struct.pack('<2f',*v) for v in vertices)
    if two_pass:payload+=struct.pack('<3I',*preliminary)
    (target/'scene-lighting.bin').write_bytes(payload)
    return dict(source_sha256=hashlib.sha256(raw).hexdigest(),ambient_rgb=ambient,
                darkness=darkness,hero_alpha=hero['a'],hero_rgb=hero_rgb,saturation=saturation,
                encoded_saturation=encoded_saturation,darkness_regions=regions,
                curve_tev=packed,curve_pre_tev=preliminary if two_pass else None,
                curve_max_error=[max(abs(tev_curve(f[1],tev_curve(pre[c],x) if two_pass else x)-
                                            max(0,min(255,curve_value(json.loads(serialized[c]),x/255)*255)))
                                     for x in range(256)) for c,f in enumerate(fits)],
                single_pass_curve_max_error=[fit_curve(keys)[1] for keys in serialized],
                grade=grade_info)
