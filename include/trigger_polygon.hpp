#pragma once
#include "physics.hpp"
#include <algorithm>
#include <vector>

namespace hk3ds {
// Optional shape refinement after a trigger's axis-aligned bounds. Each end is
// an exclusive path boundary; paths form a union, matching PolygonCollider2D.
struct TriggerPolygon {
    std::vector<Vec2> vertices;
    std::vector<unsigned> ends;
};
// Admission-only check; overlap itself stays linear in path size.
inline bool simpleTriggerPath(const std::vector<Vec2>& points,std::size_t first,std::size_t end)
{
    if(end<first||end-first<3||end-first>64||end>points.size())return false;
    const auto orient=[](Vec2 a,Vec2 b,Vec2 c){
        const double cross=(double(b.x)-a.x)*(double(c.y)-a.y)-(double(b.y)-a.y)*(double(c.x)-a.x);
        return int(cross>0)-int(cross<0);
    };
    const auto on=[](Vec2 a,Vec2 b,Vec2 p){return p.x>=std::min(a.x,b.x)&&p.x<=std::max(a.x,b.x)&&p.y>=std::min(a.y,b.y)&&p.y<=std::max(a.y,b.y);};
    const auto next=[&](std::size_t i){return i+1==end?first:i+1;};
    for(std::size_t i=first;i<end;++i){
        const auto a=points[i],b=points[next(i)],c=points[next(next(i))];
        if((a.x==b.x&&a.y==b.y)||(orient(a,b,c)==0&&
           (double(b.x)-a.x)*(double(c.x)-b.x)+(double(b.y)-a.y)*(double(c.y)-b.y)<0))return false;
        for(std::size_t j=i+1;j<end;++j){
            if(j==i+1||(i==first&&j==end-1))continue;
            const auto c=points[j],d=points[next(j)];
            const int u=orient(a,b,c),v=orient(a,b,d),w=orient(c,d,a),z=orient(c,d,b);
            if((u*v<0&&w*z<0)||(!u&&on(a,b,c))||(!v&&on(a,b,d))||(!w&&on(c,d,a))||(!z&&on(c,d,b)))return false;
        }
    }
    return true;
}
inline bool polygonTriggerOverlap(Rect body,Rect bounds,const std::vector<Vec2>& polygon,
                                  const std::vector<unsigned>& polygonEnds)
{
    if(!(body.right>bounds.left&&body.left<bounds.right&&
         body.top>bounds.bottom&&body.bottom<bounds.top))return false;
    if(polygon.empty())return true;
    // Edge clipping and point containment also handle concave source polygons.
    const Vec2 center{(body.left+body.right)*.5f,(body.bottom+body.top)*.5f};
    const auto overlaps=[&](std::size_t first,std::size_t end){
        bool contains=false;
        for(std::size_t i=first,j=end-1;i<end;j=i++)
        {
            const auto a=polygon[j],b=polygon[i];
            if((a.y>center.y)!=(b.y>center.y)&&center.x<(b.x-a.x)*(center.y-a.y)/(b.y-a.y)+a.x)contains=!contains;
            float low=0,high=1;
            const auto clip=[&](float start,float delta,float minimum,float maximum){
                if(delta==0)return start>=minimum&&start<=maximum;
                float t0=(minimum-start)/delta,t1=(maximum-start)/delta;
                if(t0>t1)std::swap(t0,t1);
                low=std::max(low,t0);high=std::min(high,t1);return low<=high;
            };
            if(clip(a.x,b.x-a.x,body.left,body.right)&&clip(a.y,b.y-a.y,body.bottom,body.top))return true;
        }
        return contains;
    };
    if(polygonEnds.empty())return overlaps(0,polygon.size());
    std::size_t first=0;
    for(unsigned end:polygonEnds){
        if(overlaps(first,end))return true;
        first=end;
    }
    return false;
}
}
