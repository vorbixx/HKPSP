"""Validate bounded, simple source PolygonCollider2D paths before admission."""
def simple_polygon_path(points):
    def orient(a,b,c):
        cross=(b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0])
        return (cross>0)-(cross<0)
    def on_segment(a,b,p):
        return min(a[0],b[0])<=p[0]<=max(a[0],b[0]) and min(a[1],b[1])<=p[1]<=max(a[1],b[1])
    def intersects(a,b,c,d):
        u,v,w,z=orient(a,b,c),orient(a,b,d),orient(c,d,a),orient(c,d,b)
        return (u*v<0 and w*z<0 or u==0 and on_segment(a,b,c) or v==0 and on_segment(a,b,d)
                or w==0 and on_segment(c,d,a) or z==0 and on_segment(c,d,b))
    n=len(points)
    if not 3<=n<=64:return False
    for i,a in enumerate(points):
        b=points[(i+1)%n];c=points[(i+2)%n]
        if a==b or orient(a,b,c)==0 and (b[0]-a[0])*(c[0]-b[0])+(b[1]-a[1])*(c[1]-b[1])<0:
            return False
        for j in range(i+1,n):
            if j==i+1 or i==0 and j==n-1:continue
            if intersects(a,b,points[j],points[(j+1)%n]):return False
    return True
