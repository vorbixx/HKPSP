#include "physics.hpp"
#include "performance_sections.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace hk3ds
{
namespace
{
float dot(Vec2 a,Vec2 b) { return a.x*b.x+a.y*b.y; }
float cross(Vec2 a,Vec2 b) { return a.x*b.y-a.y*b.x; }
Vec2 sub(Vec2 a,Vec2 b) { return {a.x-b.x,a.y-b.y}; }
// Numerical tolerances, not Unity contactOffset/edge radius.
constexpr float tolerance=0.00002f;
#ifndef HK3DS_RAY_EXHAUSTIVE
bool outsideTransverse(float p,float low,float high)
{
    // For unit cardinal rays, u is a quotient of two coordinate differences.
    // Eight epsilons cover their rounding and division at either endpoint.
    // The absolute floor also covers subnormals; overflow disables rejection.
    const float margin=8*std::numeric_limits<float>::epsilon()*
        (1+std::abs(p)+std::abs(low)+std::abs(high));
    return p<low-margin || p>high+margin;
}
#endif
bool inside(Vec2 p,const Vec2* v,std::size_t n)
{
    bool result=false;
    for (std::size_t i=0,j=n-1;i<n;j=i++)
        if ((v[i].y>p.y)!=(v[j].y>p.y) && p.x<(v[j].x-v[i].x)*(p.y-v[i].y)/(v[j].y-v[i].y)+v[i].x)
            result=!result;
    return result;
}
// Continuous SAT against an actual segment, including endpoint axes. A chain
// has zero filled area; closing an EdgeCollider does not make it a polygon.
bool sweepSegment(Vec2 center,Vec2 half,Vec2 delta,Vec2 a,Vec2 b,float& time,Vec2& normal)
{
    const auto edge=sub(b,a); const float length=std::sqrt(dot(edge,edge));
    if (length<=0) return false;
    const Vec2 axes[]={{-edge.y/length,edge.x/length},{1,0},{0,1}};
    float enter=-std::numeric_limits<float>::infinity(), leave=1;
    for (const auto axis:axes)
    {
        const float pa=dot(a,axis),pb=dot(b,axis),p=dot(center,axis);
        const float radius=std::abs(axis.x)*half.x+std::abs(axis.y)*half.y;
        const float low=std::min(pa,pb)-radius, high=std::max(pa,pb)+radius;
        const float speed=dot(delta,axis);
        if (std::abs(speed)<1e-10f)
        {
            // Tangential motion at the boundary must not hit an edge endpoint
            // at a tile seam. Gravity produces the supporting contact instead.
            if (p<=low+tolerance || p>=high-tolerance) return false;
            continue;
        }
        float near=(low-p)/speed,far=(high-p)/speed;
        Vec2 n{-axis.x,-axis.y};
        if (near>far) { std::swap(near,far); n=axis; }
        if (near>enter) { enter=near; normal=n; }
        leave=std::min(leave,far);
        if (enter>leave+tolerance) return false;
    }
    if (enter>1 || leave<0 || enter < -0.001f || dot(delta,normal)>=0) return false;
    time=std::max(0.0f,enter); return true;
}
void addContact(Contacts& c,const TerrainShape& s,Vec2 n,float impulse)
{
    // Exact normals are retained for ObjectBounce and diagnostics. Cardinal
    // flags are an adapter for the existing hero controller, not a manifold.
    if (n.y>0.5f) { c.ground=true; c.hardLanding &= !(s.flags&4); }
    if (n.y< -0.5f) c.roof=true;
    if (n.x>0.5f) c.left=true;
    if (n.x< -0.5f) c.right=true;
    if (std::abs(n.x)>0.5f) c.nonSlider |= (s.flags&2)!=0;
    for (std::size_t i=0;i<c.count;++i)
        if (c.surfaces[i].id==s.id && dot(c.surfaces[i].normal,n)>0.999f)
        { c.surfaces[i].normalImpulse+=impulse; return; }
    if (c.count==c.surfaces.size()) { c.limited=true; return; }
    c.surfaces[c.count++]={s.id,n,impulse};
}
}
#ifndef HK3DS_RAY_EXHAUSTIVE
bool terrainSensorOverlap(Rect box,const World& w)
{
    const auto intersect=[&](Rect b){
        return box.left<=b.right&&box.right>=b.left&&box.bottom<=b.top&&box.top>=b.bottom;
    };
    for(std::size_t i=0;i<w.count;++i){
        observeSimulationWork(SimulationWork::ColliderVisits);
        if(intersect(w.solids[i].bounds))return true;
    }
    for(std::size_t i=0;i<w.shapeCount;++i){
        observeSimulationWork(SimulationWork::ColliderVisits);
        const auto& s=w.shapes[i];
        if(!s.enabled||(s.flags&TerrainShape::terrainQueryExcluded)||s.count<2)continue;
        if(s.queryBoundsValid&&!intersect(s.bounds))continue;
        const auto* v=w.vertices+s.first;
        // A filled polygon can contain the complete sensor without any of
        // its edges crossing the box; an edge chain has no filled interior.
        if(!s.kind&&inside({(box.left+box.right)*.5f,(box.bottom+box.top)*.5f},v,s.count))return true;
        for(unsigned j=0;j<(s.kind?s.count-1:s.count);++j){
            const auto a=v[j],b=v[(j+1)%s.count];float enter=0,leave=1;
            const auto axis=[&](float p,float delta,float low,float high){
                if(delta==0)return p>=low&&p<=high;
                float start=(low-p)/delta,end=(high-p)/delta;
                if(start>end)std::swap(start,end);
                enter=std::max(enter,start);leave=std::min(leave,end);
                return enter<=leave;
            };
            if(axis(a.x,b.x-a.x,box.left,box.right)&&axis(a.y,b.y-a.y,box.bottom,box.top))return true;
        }
    }
    return false;
}
bool terrainQuadOverlap(const std::array<Vec2,4>& quad,const World& w)
{
    Rect bounds{quad[0].x,quad[0].y,quad[0].x,quad[0].y};
    for(const auto p:quad){bounds.left=std::min(bounds.left,p.x);bounds.right=std::max(bounds.right,p.x);
        bounds.bottom=std::min(bounds.bottom,p.y);bounds.top=std::max(bounds.top,p.y);}
    const auto broad=[&](Rect b){return bounds.left<=b.right&&bounds.right>=b.left&&
                                       bounds.bottom<=b.top&&bounds.top>=b.bottom;};
    const auto segment=[&](Vec2 a,Vec2 b){
        float enter=0,leave=1;
        for(unsigned j=0;j<quad.size();++j){
            const auto edge=sub(quad[(j+1)%quad.size()],quad[j]);
            const float start=cross(edge,sub(a,quad[j])),delta=cross(edge,sub(b,a));
            if(delta==0){if(start<0)return false;continue;}
            const float time=-start/delta;
            if(delta>0)enter=std::max(enter,time);else leave=std::min(leave,time);
            if(enter>leave)return false;
        }
        return true;
    };
    for(std::size_t i=0;i<w.count;++i){
        observeSimulationWork(SimulationWork::ColliderVisits);
        const auto b=w.solids[i].bounds;if(!broad(b))continue;
        if(quad[0].x>=b.left&&quad[0].x<=b.right&&quad[0].y>=b.bottom&&quad[0].y<=b.top)return true;
        const Vec2 v[]={{b.left,b.bottom},{b.right,b.bottom},{b.right,b.top},{b.left,b.top}};
        for(unsigned j=0;j<4;++j)if(segment(v[j],v[(j+1)%4]))return true;
    }
    for(std::size_t i=0;i<w.shapeCount;++i){
        observeSimulationWork(SimulationWork::ColliderVisits);
        const auto& s=w.shapes[i];
        if(!s.enabled||(s.flags&TerrainShape::terrainQueryExcluded)||s.count<2)continue;
        if(s.queryBoundsValid&&!broad(s.bounds))continue;
        const auto* v=w.vertices+s.first;
        if(!s.kind&&inside(quad[0],v,s.count))return true;
        for(unsigned j=0;j<(s.kind?s.count-1:s.count);++j)
            if(segment(v[j],v[(j+1)%s.count]))return true;
    }
    return false;
}
#endif
bool rayContours(Vec2 p,Vec2 direction,float length,const World& w,float& distance,std::uint32_t excludedFlags)
{
    bool hit=false;
    if (length<0) return false;
#ifndef HK3DS_RAY_EXHAUSTIVE
    const bool vertical=direction.x==0 && std::abs(direction.y)==1;
    const bool horizontal=direction.y==0 && std::abs(direction.x)==1;
#endif
    for (std::size_t i=0;i<w.shapeCount;++i)
    {
        observeSimulationWork(SimulationWork::ColliderVisits);
        const auto& s=w.shapes[i];
        if(!s.enabled || (s.flags&excludedFlags))continue;
        if ((s.flags&1) && !w.queriesHitTriggers) continue;
#ifndef HK3DS_RAY_EXHAUSTIVE
        // Cull an infinite cardinal strip, not a rounded ray endpoint. Oblique
        // and non-unit rays retain the exhaustive path, including near parallels.
        const bool outside=s.queryBoundsValid && (vertical?outsideTransverse(p.x,s.bounds.left,s.bounds.right):
            horizontal && outsideTransverse(p.y,s.bounds.bottom,s.bounds.top));
        // Preserve the floating-point polygon inside test when its horizontal
        // crossing arithmetic could extend beyond the stored X bounds.
        if(outside && (s.kind!=0 || horizontal || p.y<s.bounds.bottom || p.y>s.bounds.top))continue;
#endif
        const auto* v=w.vertices+s.first;
        if (s.kind==0 && inside(p,v,s.count))
        {
            if (w.queriesStartInside) { hit=true; distance=0; }
            continue;
        }
#ifndef HK3DS_RAY_EXHAUSTIVE
        if(outside)continue;
#endif
        const std::size_t edges=s.kind==0?s.count:s.count-1;
        for (std::size_t j=0;j<edges;++j)
        {
            const auto a=v[j],b=v[(j+1)%s.count],e=sub(b,a),ap=sub(a,p);
            const float determinant=cross(direction,e);
            if (std::abs(determinant)<1e-10f) continue;
            const float t=cross(ap,e)/determinant,u=cross(ap,direction)/determinant;
            if (t>=0 && t<=distance && u>=0 && u<=1) { hit=true; distance=t; }
        }
    }
    return hit;
}
Contacts moveContours(Vec2& p,Vec2& velocity,Rect local,float dt,const World& w,PhysicsMaterial material)
{
    Contacts contacts;
    const Vec2 offset{(local.left+local.right)/2,(local.bottom+local.top)/2};
    const Vec2 half{(local.right-local.left)/2,(local.top-local.bottom)/2};
    float remaining=std::max(0.0f,dt);
    // A bounded, allocation-free continuous solver for one non-rotating box
    // and static contours. Initial deep overlap is not resolved by this solver.
    for (int iteration=0;iteration<8 && remaining>0;++iteration)
    {
        const Vec2 center{p.x+offset.x,p.y+offset.y};
        const Vec2 delta{velocity.x*remaining,velocity.y*remaining};
        float best=2; Vec2 normal{}; const TerrainShape* shape=nullptr;
        const Rect swept{std::min(center.x,center.x+delta.x)-half.x,
                         std::min(center.y,center.y+delta.y)-half.y,
                         std::max(center.x,center.x+delta.x)+half.x,
                         std::max(center.y,center.y+delta.y)+half.y};
        for (std::size_t i=0;i<w.shapeCount;++i)
        {
            observeSimulationWork(SimulationWork::ColliderVisits);
            const auto& s=w.shapes[i]; const auto bounds=s.bounds;
            if (!s.actorContact(w.heroContacts) || swept.right<bounds.left-tolerance || swept.left>bounds.right+tolerance ||
                swept.top<bounds.bottom-tolerance || swept.bottom>bounds.top+tolerance) continue;
            const auto* v=w.vertices+s.first;
            const std::size_t edges=s.kind==0?s.count:s.count-1;
            for (std::size_t j=0;j<edges;++j)
            {
                const auto a=v[j],b=v[(j+1)%s.count]; float time; Vec2 n;
                if (!sweepSegment(center,half,delta,a,b,time,n) || time>=best) continue;
                // Filled CCW polygons expose only the exterior boundary.
                if (s.kind==0 && dot(n,{b.y-a.y,a.x-b.x})<=0) continue;
                best=time; normal=n; shape=&s;
            }
        }
        if (!shape) { p.x+=delta.x; p.y+=delta.y; remaining=0; break; }
        p.x+=delta.x*best; p.y+=delta.y*best;
        const float vn=dot(velocity,normal);
        const float impulse=std::max(0.0f,-vn);
        velocity.x+=impulse*normal.x; velocity.y+=impulse*normal.y;
        // Unity's serialized Mean friction and Maximum bounce are the only
        // accepted modes. HKT1 currently accepts zero restitution only.
        const Vec2 tangent{-normal.y,normal.x};
        const float vt=dot(velocity,tangent);
        const float friction=std::sqrt(material.friction*shape->material.friction)*impulse;
        const float reduction=std::clamp(vt,-friction,friction);
        velocity.x-=reduction*tangent.x; velocity.y-=reduction*tangent.y;
        addContact(contacts,*shape,normal,impulse);
        remaining*=1-best;
    }
    if (remaining>0) contacts.limited=true;
    return contacts;
}
}
