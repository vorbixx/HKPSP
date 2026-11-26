"""Strict solid-cell subset of the serialized TileMap mesh format.

No bounds fill: each accepted cell must be covered by two source triangles.
Only equal opaque samples can be replaced by a shared texel and merged.
"""
import base64
from collections import Counter
import math
import struct


def validate_material(material, shader):
    form = shader['m_ParsedForm']
    if form['m_Name'] != 'tk2d/BlendVertexColor' or len(form['m_SubShaders']) != 1:
        raise ValueError('Unsupported TileMap shader')
    passes = form['m_SubShaders'][0]['m_Passes']
    if len(passes) != 1: raise ValueError('Unsupported TileMap passes')
    state = passes[0]['m_State']; blend = state['m_RtBlend0']
    expected = {'m_SourceBlend':5, 'm_DestinationBlend':10, 'm_SourceBlendAlpha':5,
                'm_DestinationBlendAlpha':10, 'm_BlendOp':0, 'm_BlendOpAlpha':0, 'm_ColMask':15}
    if any(blend[k]['m_Value'] != v for k,v in expected.items()) or any(
            state[k]['m_Value'] != v for k,v in {'m_ZWrite':0,'m_ZTest':4,'m_Culling':0}.items()):
        raise ValueError('Unsupported TileMap blend/depth state')
    props = material['m_SavedProperties']; env = props['m_TexEnvs']['_MainTex']
    if (material['m_CustomRenderQueue'] != -1 or props['m_Colors'] or props['m_Floats'] or
            props.get('m_Ints') or env['m_Scale'] != {'m_X':1,'m_Y':1} or
            env['m_Offset'] != {'m_X':0,'m_Y':0} or material['m_DisabledShaderPasses']):
        raise ValueError('Unsupported TileMap material overrides')


def solid_cells(mesh, atlas):
    vd = mesh['m_VertexData']; channels = vd['m_Channels']
    expected = {0:(0,3),3:(12,4),4:(28,2)}
    for i,c in enumerate(channels):
        if c['m_Dimension'] and (i not in expected or c['m_Stream'] != 0 or c['m_Format'] != 0 or
                                (c['m_Offset'],c['m_Dimension']) != expected[i]):
            raise ValueError('Unsupported TileMap vertex layout')
    if any(not channels[i]['m_Dimension'] for i in expected): raise ValueError('Missing TileMap channel')
    raw = base64.b64decode(vd['m_Data']); ib = base64.b64decode(mesh['m_IndexBuffer'])
    count = vd['m_VertexCount']
    if len(raw) != count*36 or mesh['m_IndexFormat'] != 0 or len(ib)%12:
        raise ValueError('Unsupported TileMap buffer')
    vertices = list(struct.iter_unpack('<9f',raw)); indices = struct.unpack('<'+'H'*(len(ib)//2),ib)
    subs = mesh['m_SubMeshes']
    if len(subs) != 1 or any(subs[0][k] != v for k,v in {
            'm_Topology':0,'m_BaseVertex':0,'m_FirstByte':0,'m_FirstVertex':0,
            'm_IndexCount':len(indices),'m_VertexCount':count}.items()):
        raise ValueError('Unsupported TileMap submesh')
    if set(indices) != set(range(count)): raise ValueError('Unused/missing TileMap vertices')
    if any(not all(math.isfinite(v) for v in p) or p[2] != 0 or p[3:7] != (1,1,1,1) for p in vertices):
        raise ValueError('Unsupported TileMap depth/vertex color')
    # Include all bilinear taps, including taps just outside the UV rectangle.
    us = [p[7]*atlas.width-.5 for p in vertices]; vs = [p[8]*atlas.height-.5 for p in vertices]
    x0,x1 = math.floor(min(us)),math.floor(max(us))+1
    y0,y1 = math.floor(min(vs)),math.floor(max(vs))+1
    if x0<0 or y0<0 or x1>=atlas.width or y1>=atlas.height:
        raise ValueError('TileMap UV footprint outside atlas')
    footprint = atlas.crop((x0,atlas.height-1-y1,x1+1,atlas.height-y0))
    if footprint.getextrema() != ((0,0),(0,0),(0,0),(255,255)):
        raise ValueError('TileMap UV footprint is not opaque black')
    cells = set()
    for i in range(0,len(indices),6):
        ids = indices[i:i+6]; points = [vertices[j][:2] for j in ids]
        corners = set(points); x,y = min(p[0] for p in points),min(p[1] for p in points)
        if x != int(x) or y != int(y) or corners != {(x,y),(x+1,y),(x,y+1),(x+1,y+1)}:
            raise ValueError('TileMap triangles are not a unit grid cell')
        edges = Counter()
        for start in (0,3):
            a,b,c = points[start:start+3]
            if abs((b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0])) != 1:
                raise ValueError('Degenerate TileMap triangle')
            for u,v in ((a,b),(b,c),(c,a)): edges[tuple(sorted((u,v)))]+=1
        shared = [edge for edge,n in edges.items() if n==2]
        if len(edges)!=5 or len(shared)!=1 or any(abs(shared[0][0][j]-shared[0][1][j])!=1 for j in (0,1)):
            raise ValueError('TileMap triangles overlap or leave a hole')
        cell = (int(x),int(y))
        if cell in cells: raise ValueError('Duplicate TileMap cell')
        cells.add(cell)
    return cells


def merge_cells(cells):
    """Disjoint rectangles whose union is exactly the input cell set."""
    remaining = set(cells); rectangles = []
    while remaining:
        x,y = min(remaining,key=lambda p:(p[1],p[0])); right=x+1
        while (right,y) in remaining: right+=1
        top=y+1
        while all((xx,top) in remaining for xx in range(x,right)): top+=1
        remaining.difference_update((xx,yy) for xx in range(x,right) for yy in range(y,top))
        rectangles.append((x,y,right,top))
    return rectangles
