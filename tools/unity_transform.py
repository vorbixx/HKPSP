"""Unity Transform chain projection, shared by room visuals and scene walls.

Kept apart from convert_room_visuals so terrain (scene_walls) does not import the visual and lighting
converters: 2026-10-31 that import made every lighting edit rerun the terrain rule of all scenes.
"""


def project(point, transforms):
    x, y, z = point
    for t in transforms:
        s, q, p = t['m_LocalScale'], t['m_LocalRotation'], t['m_LocalPosition']
        x, y, z = x*s['m_X'], y*s['m_Y'], z*s['m_Z']
        a,b,c,w = q['m_X'],q['m_Y'],q['m_Z'],q['m_W']
        x,y,z = ((1-2*(b*b+c*c))*x+2*(a*b-c*w)*y+2*(a*c+b*w)*z+p['m_X'],
                 2*(a*b+c*w)*x+(1-2*(a*a+c*c))*y+2*(b*c-a*w)*z+p['m_Y'],
                 2*(a*c-b*w)*x+2*(b*c+a*w)*y+(1-2*(a*a+b*b))*z+p['m_Z'])
    return x,y,z
