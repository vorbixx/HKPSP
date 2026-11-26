#include "physics.hpp"
#include "hero_tuning.hpp"
#include "performance_sections.hpp"
#include <algorithm>
#include <cmath>

namespace hk3ds
{
Contacts solveContacts(Vec2&,Vec2&,Rect,float,const World&,PhysicsMaterial,unsigned);
Contacts moveContours(Vec2&,Vec2&,Rect,float,const World&,PhysicsMaterial);
bool rayContours(Vec2,Vec2,float,const World&,float&,std::uint32_t);
Vec2 interpolatePosition(Vec2 previous,Vec2 current,float alpha)
{
    alpha=std::clamp(alpha,0.0f,1.0f);
    return {previous.x+(current.x-previous.x)*alpha,previous.y+(current.y-previous.y)*alpha};
}
Rect bodyBounds(Vec2 p)
{
    return {p.x - tuning::bodyWidth / 2, p.y + tuning::bodyOffsetY - tuning::bodyHeight / 2,
            p.x + tuning::bodyWidth / 2, p.y + tuning::bodyOffsetY + tuning::bodyHeight / 2};
}
bool boxSensorOverlap(Rect a, Rect b)
{
    const float dx=std::max(0.0f,std::max(a.left-b.right,b.left-a.right));
    const float dy=std::max(0.0f,std::max(a.bottom-b.top,b.bottom-a.top));
    constexpr float skins=0.02f;
    return dx*dx+dy*dy<=skins*skins;
}
bool overlaps(Rect a, Rect b)
{
    return a.left < b.right && a.right > b.left && a.bottom < b.top && a.top > b.bottom;
}
Rect boxBounds(Vec2 p, Rect local, float scaleX)
{
    return {p.x+std::min(local.left*scaleX,local.right*scaleX),p.y+local.bottom,
            p.x+std::max(local.left*scaleX,local.right*scaleX),p.y+local.top};
}
Contacts moveBody(Vec2& p, Vec2& v, float dt, const World& w)
{ return moveBox(p,v,bodyBounds({}),dt,w,{},1,tuning::bodyEdgeRadius); }
Contacts moveBox(Vec2& p, Vec2& v, Rect local, float dt, const World& w, PhysicsMaterial material, unsigned colliderCopies, float edgeRadius)
{
    observeSimulationWork(SimulationWork::Bodies);
    if (w.contact.enabled)
    {
        // Radius belongs to this moving collider, never to the shared room.
        // Retain the explicit world radius used by isolated solver controls.
        World bodyWorld=w;bodyWorld.contact.bodyRadius+=edgeRadius;
        return solveContacts(p,v,local,dt,bodyWorld,material,colliderCopies);
    }
    if (w.shapeCount) return moveContours(p,v,local,dt,w,material);
    Contacts c;
    Rect a = boxBounds(p,local);
    float dx = v.x * dt;
    observeSimulationWork(SimulationWork::ColliderVisits,3*w.count);
    for (std::size_t i = 0; i < w.count; ++i)
    {
        const auto b = w.solids[i].bounds;
        if (a.bottom >= b.top || a.top <= b.bottom) continue;
        if (dx > 0 && a.right <= b.left && a.right + dx >= b.left) dx = b.left - a.right;
        if (dx < 0 && a.left >= b.right && a.left + dx <= b.right) dx = b.right - a.left;
    }
    if (dx != v.x * dt) v.x = 0;
    p.x += dx;
    a = boxBounds(p,local);
    float dy = v.y * dt;
    for (std::size_t i = 0; i < w.count; ++i)
    {
        const auto b = w.solids[i].bounds;
        if (a.left >= b.right || a.right <= b.left) continue;
        if (dy > 0 && a.top <= b.bottom && a.top + dy >= b.bottom) dy = b.bottom - a.top;
        if (dy < 0 && a.bottom >= b.top && a.bottom + dy <= b.top) dy = b.top - a.bottom;
    }
    if (dy != v.y * dt) v.y = 0;
    p.y += dy;
    a = boxBounds(p,local);
    // Tolerance is a numerical property of this solver, not a game parameter.
    constexpr float epsilon = 0.00002f;
    for (std::size_t i = 0; i < w.count; ++i)
    {
        const auto& s = w.solids[i];
        const auto b = s.bounds;
        if (a.left < b.right && a.right > b.left)
        {
            if (std::abs(a.bottom - b.top) < epsilon) { c.ground = true; c.hardLanding = s.hardLanding; }
            if (std::abs(a.top - b.bottom) < epsilon) c.roof = true;
        }
        if (a.bottom < b.top && a.top > b.bottom)
        {
            bool left = std::abs(a.left - b.right) < epsilon;
            bool right = std::abs(a.right - b.left) < epsilon;
            c.left |= left; c.right |= right;
            if (left || right) c.nonSlider |= !s.wallSlide;
        }
    }
    if ((c.right && v.x > 0) || (c.left && v.x < 0)) v.x = 0;
    if ((c.ground && v.y < 0) || (c.roof && v.y > 0)) v.y = 0;
    return c;
}
bool rayTerrain(Vec2 p, Vec2 direction, float length, const World& w, float& distance,std::uint32_t excludedFlags)
{
    distance=length;
    bool hit=rayContours(p,direction,length,w,distance,excludedFlags);
    observeSimulationWork(SimulationWork::ColliderVisits,w.count);
    for (std::size_t i=0; i<w.count; ++i)
    {
        const auto b=w.solids[i].bounds;
        if (!w.queriesStartInside && p.x>b.left && p.x<b.right && p.y>b.bottom && p.y<b.top) continue;
        float near=0, far=length;
        const auto axis=[&](float origin,float d,float low,float high)
        {
            // A ray along a surface does not enter the solid. Without this,
            // an actor resting exactly on our floor blocks its own side sweep.
            if (d==0) return origin>low && origin<high;
            float a=(low-origin)/d, z=(high-origin)/d;
            if (a>z) std::swap(a,z);
            near=std::max(near,a); far=std::min(far,z);
            return near<=far;
        };
        if (length>=0 && axis(p.x,direction.x,b.left,b.right) && axis(p.y,direction.y,b.bottom,b.top))
        { hit=true; distance=std::min(distance,near); }
    }
    return hit;
}
bool sweepBox(Vec2 p, Rect local, Vec2 d, float length, const World& w, float& distance)
{
    distance=std::max(0.0f,length);
    if (length<=0) return false;
    const Vec2 center{(local.left+local.right)/2,(local.bottom+local.top)/2};
    const Vec2 half{(local.right-local.left)/2,(local.top-local.bottom)/2};
    constexpr float skin=0.1f; // Sweep's observed default, not the body solver's skin.
    for (int i=-1; i<=1; ++i)
    {
        const Vec2 origin{p.x+center.x+half.x*(d.x+i*std::abs(d.y))-d.x*skin,
                          p.y+center.y+half.y*(d.y+i*std::abs(d.x))-d.y*skin};
        float hit;
        if (rayTerrain(origin,d,distance+skin,w,hit)) distance=std::min(distance,hit-skin);
    }
    return distance<length;
}
bool castTerrainBox(Vec2 p,Vec2 half,Vec2 d,float length,const World& w,float& distance,unsigned& flags)
{
    distance=length;flags=0;bool hit=false;
    const auto segment=[&](Vec2 a,Vec2 b,unsigned fixtureFlags)
    {
        float enter=0,leave=distance;
        const auto axis=[&](Vec2 n)
        {
            const float x=a.x*n.x+a.y*n.y,y=b.x*n.x+b.y*n.y;
            const float radius=half.x*std::abs(n.x)+half.y*std::abs(n.y);
            const float low=std::min(x,y)-radius,high=std::max(x,y)+radius;
            const float origin=p.x*n.x+p.y*n.y,speed=d.x*n.x+d.y*n.y;
            if(speed==0)return origin>=low&&origin<=high;
            float start=(low-origin)/speed,end=(high-origin)/speed;
            if(start>end)std::swap(start,end);
            enter=std::max(enter,start);leave=std::min(leave,end);
            return enter<=leave;
        };
        if(axis({1,0})&&axis({0,1})&&axis({a.y-b.y,b.x-a.x})&&(!hit||enter<distance))
        {hit=true;distance=enter;flags=fixtureFlags;}
    };
    observeSimulationWork(SimulationWork::ColliderVisits,w.shapeCount+w.count);
    for(std::size_t i=0;i<w.shapeCount;++i)
    {
        // This is HeroController.CheckForTerrainThunk's layer 8/25 cast.
        // Collision-matrix exclusions and query layers are independent policies.
        const auto& s=w.shapes[i];if(!s.enabled || (s.flags&TerrainShape::nailQueryExcluded))continue;
        const auto* v=w.vertices+s.first;
        for(unsigned j=0;j<(s.kind?s.count-1:s.count);++j)segment(v[j],v[(j+1)%s.count],s.flags);
    }
    for(std::size_t i=0;i<w.count;++i)
    {
        const auto r=w.solids[i].bounds;
        const Vec2 v[]={{r.left,r.bottom},{r.right,r.bottom},{r.right,r.top},{r.left,r.top}};
        for(unsigned j=0;j<4;++j)segment(v[j],v[(j+1)%4],0);
    }
    return hit;
}
}
