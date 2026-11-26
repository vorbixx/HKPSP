#ifdef __3DS__
#include "frame_profile.hpp"
#endif
#include "physics.hpp"
#include "performance_sections.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace hk3ds
{
namespace
{
// These internal solver tolerances are inferred from the original controls,
// including varied iterations/baumgarte/TOI. They are not a fitted floor gap.
constexpr float slop=0.005f;
constexpr float epsilon=0.000002f;
float dot(Vec2 a,Vec2 b) { return a.x*b.x+a.y*b.y; }
Vec2 sub(Vec2 a,Vec2 b) { return {a.x-b.x,a.y-b.y}; }
Vec2 plus(Vec2 a,Vec2 b) { return {a.x+b.x,a.y+b.y}; }
Vec2 mul(Vec2 a,float s) { return {a.x*s,a.y*s}; }
float support(Vec2 n,Vec2 half) { return std::abs(n.x)*half.x+std::abs(n.y)*half.y; }
unsigned boxFeature(Vec2 p,Vec2 half,bool vertex)
{
    if(vertex)return p.y<0?(p.x<0?0:1):(p.x<0?3:2);
    return std::abs(p.x)>=half.x-epsilon?(p.x<0?4:5):(p.y<0?6:7);
}
struct Manifold
{
    const TerrainShape* shape=nullptr;
    Vec2 normal{};
    // Point offsets from box center, and fixed surface plane for this normal.
    std::array<Vec2,2> points{};
    std::array<float,2> pointPlanes{};
    std::array<Vec2,2> worldPoints{};
    std::array<std::uint32_t,2> features{};
    unsigned child=0;
    float radius=0, impulse=0, tangentImpulse=0, separation=0;
    unsigned count=0; bool toi=false, endpoint=false, corner=false;
    Vec2 anchor{};
};
float separation(const Manifold& m,Vec2 center,unsigned i)
{
    if(m.corner){const auto d=sub(plus(center,m.points[i]),m.anchor);return std::sqrt(dot(d,d))-m.radius;}
    return dot(plus(center,m.points[i]),m.normal)-m.pointPlanes[i]-m.radius;
}

bool manifold(Vec2 center,Vec2 half,Vec2 a,Vec2 b,const TerrainShape& s,
              float radius,Manifold& m,const Vec2* chain=nullptr,unsigned segment=0,bool rounded=false)
{
    const auto edge=sub(b,a); const float length=std::sqrt(dot(edge,edge));
    if (length<=0) return false;
    const Vec2 face{-edge.y/length,edge.x/length};
    // An explicit rounded box has vertex contacts. Contact offset alone
    // retains the polygon clipping path (as in the source zero-radius controls).
    if(rounded)for(unsigned end=0;end<2;++end)
    {
        const Vec2 anchor=end?b:a;
        const Vec2 point{std::clamp(anchor.x,center.x-half.x,center.x+half.x),
                         std::clamp(anchor.y,center.y-half.y,center.y+half.y)};
        const auto d=sub(point,anchor);
        if(std::abs(d.x)<=epsilon || std::abs(d.y)<=epsilon)continue;
        const float along=dot(sub(point,a),edge);
        if(end?along<length*length:along>0)continue;
        const float distance=std::sqrt(dot(d,d));if(distance>radius+epsilon)continue;
        const Vec2 n=mul(d,1/distance);
        if(s.kind==0 && dot(n,face)>=0)continue;
        if(s.kind==1 && chain)
        {
            const unsigned vertex=segment+end;
            if(vertex>0 && vertex+1<s.count)
            {
                auto e0=sub(chain[vertex],chain[vertex-1]),e1=sub(chain[vertex+1],chain[vertex]);
                Vec2 n0{-e0.y,e0.x},n1{-e1.y,e1.x};
                if(dot(sub(center,anchor),face)<0){n0=mul(n0,-1);n1=mul(n1,-1);}
                const auto cross=[](Vec2 x,Vec2 y){return x.x*y.y-x.y*y.x;};
                if(cross(n0,n1)<0)std::swap(n0,n1);
                if(cross(n0,n)<-epsilon || cross(n,n1)<-epsilon)continue;
            }
        }
        m={};m.shape=&s;m.normal=n;m.radius=radius;m.child=segment;
        m.count=1;m.endpoint=true;m.corner=true;m.anchor=anchor;
        m.points[0]=sub(point,center);m.pointPlanes[0]=dot(anchor,n);
        m.features[0]=end|(boxFeature(m.points[0],half,true)<<4);
        m.separation=distance-radius;m.worldPoints[0]=mul(plus(point,anchor),.5f);
        return true;
    }
    const Vec2 axes[]={face,{1,0},{0,1}};
    float best=-std::numeric_limits<float>::infinity(); unsigned chosen=0;
    Vec2 normal{}; float plane=0;
    for (unsigned k=0;k<3;++k)
    {
        const auto n=axes[k]; const float pa=dot(a,n),pb=dot(b,n),p=dot(center,n);
        const float extent=support(n,half);
        const float above=p-extent-std::max(pa,pb),below=std::min(pa,pb)-p-extent;
        const float gap=std::max(above,below);
        if (gap>radius+epsilon) return false;
        if (gap>best+epsilon)
        { best=gap; chosen=k; normal=above>=below?n:mul(n,-1); plane=above>=below?std::max(pa,pb):-std::min(pa,pb); }
    }
    // At an internal chain vertex, adjacent faces bound the normal cone.
    // An endpoint axis outside that cone would create a ghost lateral wall.
    if(s.kind==1 && chosen!=0 && chain)
    {
        const bool atStart=dot(a,normal)>=dot(b,normal);
        const unsigned vertex=atStart?segment:segment+1;
        if(vertex>0 && vertex+1<s.count)
        {
            const auto e0=sub(chain[vertex],chain[vertex-1]),e1=sub(chain[vertex+1],chain[vertex]);
            const float l0=std::sqrt(dot(e0,e0)),l1=std::sqrt(dot(e1,e1));
            Vec2 n0{-e0.y/l0,e0.x/l0},n1{-e1.y/l1,e1.x/l1};
            if(dot(sub(center,chain[vertex]),face)<0){n0=mul(n0,-1);n1=mul(n1,-1);}
            const auto cross=[](Vec2 x,Vec2 y){return x.x*y.y-x.y*y.x;};
            if(cross(n0,n1)<0)std::swap(n0,n1);
            if(cross(n0,normal)<-epsilon || cross(normal,n1)<-epsilon)return false;
        }
    }
    // Filled CCW contours have exterior faces only. Edge chains are two-sided.
    if (s.kind==0 && dot(normal,face)>=0) return false;
    m={}; m.shape=&s; m.normal=normal; m.radius=radius; m.child=segment;
    m.endpoint=chosen!=0;
    if (chosen!=0)
    {
        // Clip the incident terrain edge against the reference box face.
        // An endpoint outside its side planes does not form a contact; a
        // short overlap may retain both the endpoint and a clipped point.
        const Vec2 tangent{-normal.y,normal.x};
        const float extent=support(tangent,half);
        const float t0=dot(sub(a,center),tangent),t1=dot(sub(b,center),tangent);
        float u0=0,u1=1;
        if(std::abs(t1-t0)>epsilon)
        {
            float v0=(-extent-t0)/(t1-t0),v1=(extent-t0)/(t1-t0);
            if(v0>v1)std::swap(v0,v1);
            u0=std::max(u0,v0);u1=std::min(u1,v1);
        }
        else if(t0< -extent || t0>extent)return false;
        if(u0>u1)return false;
        for(float u:{u0,u1})
        {
            const Vec2 incident=plus(a,mul(edge,u));
            const float pointPlane=dot(incident,normal);
            const Vec2 point=plus(mul(normal,-support(normal,half)),mul(tangent,dot(sub(incident,center),tangent)));
            if(dot(plus(center,point),normal)-pointPlane>radius+epsilon)continue;
            if(m.count && dot(sub(point,m.points[0]),sub(point,m.points[0]))<epsilon*epsilon)continue;
            m.pointPlanes[m.count]=pointPlane;
            // Terrain vertex/box face or terrain face/box vertex. The bit
            // layout is our own stable geometric identity for correspondence.
            const bool clipped=u>0 && u<1;
            m.features[m.count]=(u<=0?0u:u>=1?1u:2u) | (boxFeature(point,half,clipped)<<4);
            m.points[m.count++]=point;
        }
    }
    else
    {
        Vec2 p0,p1;
        if (std::abs(normal.y)>=std::abs(normal.x))
        { p0={-half.x,normal.y>0?-half.y:half.y}; p1={half.x,p0.y}; }
        else
        { p0={normal.x>0?-half.x:half.x,-half.y}; p1={p0.x,half.y}; }
        const Vec2 tangent{edge.x/length,edge.y/length};
        const float lo=dot(a,tangent),hi=dot(b,tangent);
        const float t0=dot(plus(center,p0),tangent),t1=dot(plus(center,p1),tangent);
        float u0=0,u1=1;
        if (std::abs(t1-t0)>epsilon)
        {
            float v0=(lo-t0)/(t1-t0),v1=(hi-t0)/(t1-t0);
            if(v0>v1)std::swap(v0,v1);
            u0=std::max(u0,v0);u1=std::min(u1,v1);
        }
        else if(t0<lo-epsilon || t0>hi+epsilon)return false;
        if(u0>u1+epsilon)return false;
        const auto span=sub(p1,p0);
        for(float u:{u0,u1})
        {
            const auto point=plus(p0,mul(span,u));
            if(dot(plus(center,point),normal)-plane>radius+epsilon)continue;
            if(m.count && dot(sub(point,m.points[0]),sub(point,m.points[0]))<epsilon*epsilon)continue;
            m.pointPlanes[m.count]=plane;
            const bool clipped=u>0 && u<1;
            const unsigned terrainFeature=clipped?(dot(plus(center,point),tangent)>(lo+hi)/2?1u:0u):2u;
            m.features[m.count]=terrainFeature | (boxFeature(point,half,!clipped)<<4);
            m.points[m.count++]=point;
        }
    }
    if(!m.count)return false;
    m.separation=separation(m,center,0);
    for(unsigned i=0;i<m.count;++i)
    {
        const auto boxPoint=plus(center,m.points[i]);
        m.worldPoints[i]=sub(boxPoint,mul(normal,(separation(m,center,i)+radius)*0.5f));
    }
    return true;
}
template<std::size_t Capacity>
struct ManifoldBatch { std::array<Manifold,Capacity> items{}; std::size_t count=0; bool limited=false; };
using Manifolds=ManifoldBatch<32>;
template<std::size_t Capacity>
void append(ManifoldBatch<Capacity>& all,const Manifold& m)
{
    // Each chain child keeps its own constraints, including at a shared vertex.
    if(all.count==all.items.size()){all.limited=true;return;}
    all.items[all.count++]=m;
}
// One move collects contacts and then queries CCD, usually over the same terrain.
// Keep original fixture order and bound only the centers we can reuse safely.
struct ContactCandidates
{
    std::array<std::size_t,64> indices;
    Rect centers{};
    std::size_t count=0;
    bool cached=false;
    ContactCandidates(Vec2 center,Vec2 half,const World& w)
    {
#ifndef HK3DS_CCD_EXHAUSTIVE
        if(!w.contact.continuous || !std::isfinite(center.x) || !std::isfinite(center.y) ||
           !std::isfinite(half.x) || !std::isfinite(half.y) || half.x<0 || half.y<0)return;
        centers={center.x-2,center.y-2,center.x+2,center.y+2};
        for(std::size_t i=0;i<w.shapeCount;++i)
        {
            const auto& s=w.shapes[i];if(!s.actorContact(w.heroContacts))continue;
            const float radius=2*w.contact.offset+w.contact.bodyRadius+s.radius;
            const float margin=radius+slop;
            if(centers.right+half.x+margin<s.bounds.left || centers.left-half.x-margin>s.bounds.right ||
               centers.top+half.y+margin<s.bounds.bottom || centers.bottom-half.y-margin>s.bounds.top)continue;
            if(count==indices.size())
            {
                observeSimulationWork(SimulationWork::ColliderVisits,i+1);
                return;
            }
            indices[count++]=i;
        }
        observeSimulationWork(SimulationWork::ColliderVisits,w.shapeCount);
        cached=true;
#else
        (void)center;(void)half;(void)w;
#endif
    }
    bool contains(Vec2 a,Vec2 b)const
    {
        return cached && a.x>=centers.left && a.x<=centers.right &&
            a.y>=centers.bottom && a.y<=centers.top &&
            b.x>=centers.left && b.x<=centers.right && b.y>=centers.bottom && b.y<=centers.top;
    }
};
Manifolds collect(Vec2 center,Vec2 half,const World& w,Vec2 velocity,const ContactCandidates& candidates)
{
    Manifolds all;
    const auto count=candidates.cached?candidates.count:w.shapeCount;
    observeSimulationWork(SimulationWork::ColliderVisits,count);
    for(std::size_t k=0;k<count;++k)
    {
        const auto i=candidates.cached?candidates.indices[k]:k;
        const auto& s=w.shapes[i]; if(!s.actorContact(w.heroContacts))continue;
        const float radius=2*w.contact.offset+w.contact.bodyRadius+s.radius;
        if(center.x+half.x+radius<s.bounds.left || center.x-half.x-radius>s.bounds.right ||
           center.y+half.y+radius<s.bounds.bottom || center.y-half.y-radius>s.bounds.top)continue;
        const auto* v=w.vertices+s.first; const auto edges=s.kind==0?s.count:s.count-1;
        for(std::size_t j=0;j<edges;++j)
        { Manifold m; if(manifold(center,half,v[j],v[(j+1)%s.count],s,radius,m,v,static_cast<unsigned>(j),w.contact.bodyRadius>0) && !(m.endpoint && dot(velocity,m.normal)>epsilon))append(all,m); }
    }
    return all;
}
template<std::size_t Capacity>
void velocitySolve(ManifoldBatch<Capacity>& all,Vec2& velocity,PhysicsMaterial material,unsigned iterations)
{
    for(unsigned k=0;k<iterations;++k)
    for(std::size_t i=0;i<all.count;++i)
    {
        auto& m=all.items[i];
        const Vec2 t{-m.normal.y,m.normal.x};
        const float cap=std::sqrt(material.friction*m.shape->material.friction)*m.impulse;
        const float oldTangent=m.tangentImpulse;
        m.tangentImpulse=std::clamp(oldTangent-dot(velocity,t),-cap,cap);
        velocity=plus(velocity,mul(t,m.tangentImpulse-oldTangent));
        const float old=m.impulse;
        m.impulse=std::max(0.0f,old-dot(velocity,m.normal));
        velocity=plus(velocity,mul(m.normal,m.impulse-old));
    }
}
template<std::size_t Capacity>
void positionSolve(ManifoldBatch<Capacity>& all,Vec2& center,float scale,float maximum,unsigned iterations,float exitSlop)
{
    for(unsigned k=0;k<iterations;++k)
    {
        float minimum=0;
        for(std::size_t i=0;i<all.count;++i)
        {
            auto& m=all.items[i];
            for(unsigned j=0;j<m.count;++j)
            {
                if(m.corner){const auto d=sub(plus(center,m.points[j]),m.anchor);const float len=std::sqrt(dot(d,d));if(len>epsilon)m.normal=mul(d,1/len);}
                const float sep=separation(m,center,j);minimum=std::min(minimum,sep);
                const float correction=std::clamp(scale*(sep+slop),-maximum,0.0f);
                center=sub(center,mul(m.normal,correction));
            }
        }
        if(minimum>=-exitSlop)break;
    }
}
// Non-rotating shape TOI: SAT sweep to the core-distance target. The target
// must still produce a padded manifold; tiny-radius controls do not do so.
bool impact(Vec2 center,Vec2 half,Vec2 delta,Vec2 a,Vec2 b,const TerrainShape& s,
            float radius,float& time,Manifold& m,const Vec2* chain,unsigned segment,bool rounded)
{
    const auto edge=sub(b,a);const float length=std::sqrt(dot(edge,edge));if(length<=0)return false;
    const Vec2 axes[]={{-edge.y/length,edge.x/length},{1,0},{0,1}};
    const float target=std::max(slop,radius-3*slop);
    float enter=0,leave=1;
    for(const auto n:axes)
    {
        const float pa=dot(a,n),pb=dot(b,n),p=dot(center,n),speed=dot(delta,n);
        const float extent=support(n,half)+target;
        const float lo=std::min(pa,pb)-extent,hi=std::max(pa,pb)+extent;
        if(std::abs(speed)<1e-10f){if(p<lo || p>hi)return false;continue;}
        float near=(lo-p)/speed,far=(hi-p)/speed;if(near>far)std::swap(near,far);
        enter=std::max(enter,near);leave=std::min(leave,far);if(enter>leave)return false;
    }
    if(enter>1 || leave<0)return false;
    if(rounded)
    {
        // Refine square SAT bounds with vertex-distance conservative advance.
        // The source controls stop within 1/4 linear slop of the TOI target;
        // an exact circle root changes the outgoing velocity at grazing hits.
        Manifold initial;
        if(manifold(plus(center,mul(delta,enter)),half,a,b,s,radius,initial,chain,segment,true) && initial.corner)
        {
            float hit=0;
            for(unsigned iteration=0;iteration<20;++iteration)
            {
                const auto d=sub(plus(plus(center,mul(delta,hit)),initial.points[0]),initial.anchor);
                const float distance=std::sqrt(dot(d,d));
                if(distance<=target+.25f*slop){enter=hit;break;}
                const float speed=dot(delta,mul(d,1/distance));
                if(speed>=0)return false;
                hit+=(target-distance)/speed;
                if(hit>leave || hit>1 || iteration==19)return false;
            }
        }
    }
    if(!manifold(plus(center,mul(delta,enter)),half,a,b,s,radius,m,chain,segment,rounded))return false;
    // Separating bodies should not be brought back onto their support plane.
    if(enter>0 && dot(delta,m.normal)>epsilon)return false;
    if(enter==0 && m.separation+radius<=0)return false;
    if(m.separation>=-1.5f*slop)return false;
    time=enter;m.toi=true;return true;
}
void record(Contacts& c,const Manifold& m)
{
    const auto& s=*m.shape;const auto n=m.normal;
    if(n.y>0.5f){c.ground=true;c.hardLanding &= !(s.flags&4);}
    if(n.y< -0.5f)c.roof=true;
    if(n.x>0.5f)c.left=true;
    if(n.x< -0.5f)c.right=true;
    if(std::abs(n.x)>0.5f)c.nonSlider |= (s.flags&2)!=0;
    for(std::size_t i=0;i<c.count;++i)
        if(c.surfaces[i].id==s.id && dot(c.surfaces[i].normal,n)>0.99999f)
        {
            auto& c0=c.surfaces[i];
            if(m.toi && !c0.toi)c0={s.id,n,m.impulse,m.separation,m.count,true,s.flags};
            else { c0.normalImpulse+=m.impulse;c0.points+=m.count; }
            return;
        }
    if(c.count==c.surfaces.size()){c.limited=true;return;}
    c.surfaces[c.count++]={s.id,n,m.impulse,m.separation,m.count,m.toi,s.flags};
}
void diagnose(ContactDiagnostics* d,const Manifold& m)
{
    if(!d)return;
    if(m.toi)
    {
        // A TOI update replaces the same child manifold returned by GetContacts.
        for(std::size_t i=0;i<d->count;)
            if(d->points[i].shape==m.shape->id && d->points[i].child==m.child)
            {for(std::size_t j=i+1;j<d->count;++j)d->points[j-1]=d->points[j];--d->count;}
            else ++i;
    }
    for(unsigned i=0;i<m.count;++i)
    {
        if(d->count==d->capacity){d->limited=true;return;}
        d->points[d->count++]={m.shape->id,m.child,m.features[i],m.worldPoints[i],m.normal,
            m.separation+dot(sub(m.points[i],m.points[0]),m.normal)-m.pointPlanes[i]+m.pointPlanes[0],m.toi};
    }
}

}
Contacts solveContacts(Vec2& p,Vec2& velocity,Rect local,float dt,const World& w,PhysicsMaterial material,unsigned colliderCopies)
{
#ifdef __3DS__
    FrameProfile::Detail contactTiming(frameProfile,0);
#endif
    if(w.contact.diagnostics){w.contact.diagnostics->count=0;w.contact.diagnostics->limited=false;}
    Contacts contacts;if(dt<=0)return contacts;
    const Vec2 offset{(local.left+local.right)/2,(local.bottom+local.top)/2};
    const Vec2 half{(local.right-local.left)/2,(local.top-local.bottom)/2};
    Vec2 center=plus(p,offset);const auto start=center;
    const ContactCandidates candidates(center,half,w);
    auto all=collect(center,half,w,velocity,candidates);
    if(!colliderCopies || colliderCopies>2 || all.limited || all.count*colliderCopies>all.items.size())
    {contacts.limited=true;return contacts;}
    const auto originalCount=all.count;
    for(unsigned copy=1;copy<colliderCopies;++copy)
        for(std::size_t i=0;i<originalCount;++i)all.items[all.count++]=all.items[i];
    velocitySolve(all,velocity,material,w.contact.velocityIterations);
    const float speed=std::sqrt(dot(velocity,velocity));
    const float limit=w.contact.maxTranslation/dt;
    if(speed>limit)velocity=mul(velocity,limit/speed);
    const auto delta=mul(velocity,dt);center=plus(center,delta);
    positionSolve(all,center,w.contact.baumgarte,w.contact.maxCorrection,w.contact.positionIterations,3*slop);
    for(std::size_t i=0;i<all.count;++i){record(contacts,all.items[i]);diagnose(w.contact.diagnostics,all.items[i]);}
    if(w.contact.continuous)
    {
        // Bounded CCD events; supporting contacts remain in the final result.
        Vec2 sweepStart=start,sweepDelta=sub(center,start);float remaining=dt;
#ifdef __3DS__
        FrameProfile::Detail ccdTiming(frameProfile,1);
#endif
        for(unsigned step=0;step<8;++step)
        {
            float best=2;Manifold chosen;
            const auto end=plus(sweepStart,sweepDelta);
            const bool cached=candidates.contains(sweepStart,end);
            const auto count=cached?candidates.count:w.shapeCount;
            observeSimulationWork(SimulationWork::ColliderVisits,count);
            for(std::size_t k=0;k<count;++k)
            {
                const auto i=cached?candidates.indices[k]:k;
                const auto& s=w.shapes[i];if(!s.actorContact(w.heroContacts))continue;
                const auto* v=w.vertices+s.first;const auto edges=s.kind==0?s.count:s.count-1;
                const float radius=2*w.contact.offset+w.contact.bodyRadius+s.radius;
#ifndef HK3DS_CCD_EXHAUSTIVE
                // Conservative swept bounds, recomputed after every TOI response.
                // Keep edge order and the narrow phase unchanged for candidates.
                const float margin=radius+slop;
                if(std::max(sweepStart.x,end.x)+half.x+margin<s.bounds.left ||
                   std::min(sweepStart.x,end.x)-half.x-margin>s.bounds.right ||
                   std::max(sweepStart.y,end.y)+half.y+margin<s.bounds.bottom ||
                   std::min(sweepStart.y,end.y)-half.y-margin>s.bounds.top)continue;
#endif
                for(std::size_t j=0;j<edges;++j)
                {
                    float time;Manifold m;
                    if(impact(sweepStart,half,sweepDelta,v[j],v[(j+1)%s.count],s,radius,time,m,v,static_cast<unsigned>(j),w.contact.bodyRadius>0) && time<best)
                    {best=time;chosen=m;}
                }
            }
            if(best>1)break;
            center=plus(sweepStart,mul(sweepDelta,best));
            // Initial contacts are already recorded. Reuse their scratch buffer
            // so CCD does not keep two full manifold arrays on the ARM stack.
            auto& toi=all;toi.count=0;toi.limited=false;append(toi,chosen);
            for(unsigned copy=1;copy<colliderCopies;++copy)toi.items[toi.count++]=chosen;
            positionSolve(toi,center,w.contact.toiBaumgarte,w.contact.maxCorrection,20,1.5f*slop);
            velocitySolve(toi,velocity,material,w.contact.velocityIterations);
            record(contacts,toi.items[0]);
            diagnose(w.contact.diagnostics,toi.items[0]);
            remaining*=1-best;
            sweepStart=center;sweepDelta=mul(velocity,remaining);
            center=plus(sweepStart,sweepDelta);
            if(step==7)contacts.limited=true;
        }
    }
    p=sub(center,offset);return contacts;
}
}
