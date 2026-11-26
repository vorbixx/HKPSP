#pragma once
#include "physics.hpp"
#include "performance_sections.hpp"
#include <algorithm>
#include <array>
#include <cmath>

namespace hk3ds
{
// Decorative approximation only. Swept circles against enabled room contours;
// no Unity fixture ordering, native broadphase or solver equivalence is claimed.
namespace decorTerrain
{
inline Vec2 add(Vec2 a,Vec2 b){return {a.x+b.x,a.y+b.y};}
inline Vec2 sub(Vec2 a,Vec2 b){return {a.x-b.x,a.y-b.y};}
inline Vec2 mul(Vec2 a,float s){return {a.x*s,a.y*s};}
inline float dot(Vec2 a,Vec2 b){return a.x*b.x+a.y*b.y;}
// Per-move broadphase only: no persistent state, heap allocation or reordered
// contacts. A separation/ricochet outside the envelope uses the full scan.
struct FixtureQuery
{
    std::array<std::size_t,64> indices;
    std::size_t count=0;
    Rect envelope;
    bool complete=false;
    FixtureQuery(const World& world,Rect bounds):envelope(bounds)
    {
        if(!std::isfinite(bounds.left)||!std::isfinite(bounds.bottom)||
           !std::isfinite(bounds.right)||!std::isfinite(bounds.top))return;
        std::size_t tests=0;
        const auto append=[&](std::size_t index){
            if(count==indices.size()){
                observeSimulationWork(SimulationWork::DecorTerrainTests,tests);return false;
            }
            indices[count++]=index;return true;
        };
        for(std::size_t i=0;i<world.shapeCount;++i){
            ++tests;
            const auto& s=world.shapes[i];
            if(s.enabled&&!(s.flags&1)&&s.count>=2&&overlaps(bounds,s.bounds)&&!append(i))return;
        }
        for(std::size_t i=0;i<world.count;++i){
            ++tests;
            if(overlaps(bounds,world.solids[i].bounds)&&!append(world.shapeCount+i))return;
        }
        observeSimulationWork(SimulationWork::DecorTerrainTests,tests);complete=true;
    }
    bool covers(Rect bounds)const
    {
        return complete&&bounds.left>=envelope.left&&bounds.bottom>=envelope.bottom&&
            bounds.right<=envelope.right&&bounds.top<=envelope.top;
    }
};
template<class F> inline void fixtures(const World& world,Rect area,F visit,const FixtureQuery* query=nullptr)
{
    observeSimulationWork(SimulationWork::DecorTerrainQueries);
    const auto shape=[&](std::size_t i){
        const auto& s=world.shapes[i];
        if(!s.enabled||(s.flags&1)||s.count<2||!overlaps(area,s.bounds))return;
        visit(world.vertices+s.first,s.count,s.kind==0);
    };
    const auto solid=[&](std::size_t i){
        const auto r=world.solids[i].bounds;if(!overlaps(area,r))return;
        const Vec2 v[]={{r.left,r.bottom},{r.right,r.bottom},{r.right,r.top},{r.left,r.top}};
        visit(v,4,true);
    };
    if(query&&query->covers(area)){
        observeSimulationWork(SimulationWork::DecorTerrainTests,query->count);
        for(std::size_t j=0;j<query->count;++j){
            const auto i=query->indices[j];
            if(i<world.shapeCount)shape(i);else solid(i-world.shapeCount);
        }
    }else{
        observeSimulationWork(SimulationWork::DecorTerrainTests,world.shapeCount+world.count);
        for(std::size_t i=0;i<world.shapeCount;++i)shape(i);
        for(std::size_t i=0;i<world.count;++i)solid(i);
    }
}
inline Rect area(Vec2 a,Vec2 b,float r)
{return {std::min(a.x,b.x)-r,std::min(a.y,b.y)-r,std::max(a.x,b.x)+r,std::max(a.y,b.y)+r};}
inline Vec2 closest(Vec2 p,Vec2 a,Vec2 b)
{
    const auto edge=sub(b,a);const float n=dot(edge,edge);
    return add(a,mul(edge,n>0?std::clamp(dot(sub(p,a),edge)/n,0.0f,1.0f):0));
}
inline Vec2 separate(Vec2 p,float radius,const World& world,const FixtureQuery* query=nullptr)
{
    // Births can intersect the terrain; size-over-life can also grow a circle.
    for(unsigned pass=0;pass<4;++pass){
        bool moved=false;
        fixtures(world,area(p,p,radius),[&](const Vec2* v,unsigned count,bool filled){
            bool inside=false;float best=1e30f;Vec2 nearest{},outward{};
            for(unsigned j=0;j<(filled?count:count-1);++j){
                const auto a=v[j],b=v[(j+1)%count],edge=sub(b,a);
                if(filled&&(a.y>p.y)!=(b.y>p.y)&&p.x<(b.x-a.x)*(p.y-a.y)/(b.y-a.y)+a.x)inside=!inside;
                const auto q=closest(p,a,b),d=sub(p,q);const float d2=dot(d,d);
                if(d2<best){best=d2;nearest=q;const float len=std::sqrt(dot(edge,edge));
                    outward=len>0?Vec2{edge.y/len,-edge.x/len}:Vec2{0,1};}
            }
            if(!inside&&best>=radius*radius)return;
            const float distance=std::sqrt(best);
            const auto n=distance>1e-7f?mul(sub(p,nearest),(inside?-1.0f:1.0f)/distance):outward;
            p=add(nearest,mul(n,radius+.0001f));moved=true;
        },query);
        if(!moved)break;
    }
    return p;
}
struct Hit {float fraction=1;Vec2 normal{};bool hit=false;};
inline Hit cast(Vec2 p,Vec2 delta,float radius,const World& world,const FixtureQuery* query=nullptr)
{
    Hit hit;
    const auto accept=[&](float t,Vec2 n){
        if(t>=0&&t<=hit.fraction&&dot(delta,n)<-1e-7f){hit={t,n,true};}
    };
    const auto swept=area(p,add(p,delta),radius+.0001f);
    fixtures(world,swept,[&](const Vec2* v,unsigned count,bool filled){
        for(unsigned j=0;j<(filled?count:count-1);++j){
            const auto a=v[j],b=v[(j+1)%count];
            // A contour can span the whole room. Reject distant segments before
            // normalizing edges or solving endpoint quadratics.
            if(!overlaps(swept,area(a,b,.0001f)))continue;
            const auto edge=sub(b,a);const float len=std::sqrt(dot(edge,edge));
            if(len<=1e-7f)continue;
            const Vec2 tangent=mul(edge,1/len),normal{tangent.y,-tangent.x};
            for(unsigned side=0;side<(filled?1u:2u);++side){
                const auto n=mul(normal,side?-1.0f:1.0f);const float speed=dot(delta,n);
                if(speed>=-1e-7f)continue;
                const float t=(radius-dot(sub(p,a),n))/speed;
                const float along=dot(sub(add(p,mul(delta,t)),a),tangent);
                if(along>=0&&along<=len)accept(t,n);
            }
            const float speed2=dot(delta,delta);if(speed2<=1e-14f)continue;
            for(auto endpoint:{a,b}){
                const auto offset=sub(p,endpoint);const float bq=dot(offset,delta);
                const float discriminant=bq*bq-speed2*(dot(offset,offset)-radius*radius);
                if(discriminant<0)continue;
                const float t=(-bq-std::sqrt(discriminant))/speed2;
                if(t>=0&&t<=hit.fraction)accept(t,mul(sub(add(p,mul(delta,t)),endpoint),1/radius));
            }
        }
    },query);
    return hit;
}
inline Vec2 reflect(Vec2 v,Vec2 normal,float bounce,float retention)
{
    const float into=std::min(0.0f,dot(v,normal));
    return mul(sub(v,mul(normal,(1+bounce)*into)),retention);
}
inline bool move(Vec2& p,Vec2 destination,Vec2& velocity,float radius,float bounce,float retention,const World& world)
{
    const FixtureQuery query(world,area(p,destination,radius+.0001f));
    const Vec2 original=p;p=separate(p,radius,world,&query);
    Vec2 remaining=sub(destination,original);bool touched=false;
    for(unsigned iteration=0;iteration<4;++iteration){
        const auto hit=cast(p,remaining,radius,world,&query);
        if(!hit.hit){p=add(p,remaining);break;}
        touched=true;p=add(add(p,mul(remaining,hit.fraction)),mul(hit.normal,.0001f));
        velocity=reflect(velocity,hit.normal,bounce,retention);
        remaining=reflect(mul(remaining,1-hit.fraction),hit.normal,bounce,retention);
    }
    p=separate(p,radius,world,&query);return touched;
}
}
}
