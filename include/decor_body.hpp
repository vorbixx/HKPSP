#pragma once
#include "decor_terrain.hpp"
#include <array>

namespace hk3ds::decorBody {
// Bounded decorative polygon contacts, not Unity's fixture/solver ordering.
// Only the two Great Door rigidbodies use this; ParticleSystems retain circles.
inline float cross(Vec2 a,Vec2 b){return a.x*b.y-a.y*b.x;}
inline Vec2 rotate(Vec2 p,float angle)
{const float c=std::cos(angle),s=std::sin(angle);return {c*p.x-s*p.y,s*p.x+c*p.y};}
inline Vec2 centroid(const std::array<float,17>& hull){return {hull[1],hull[2]};}
struct Contact {bool hit=false;float depth=0;Vec2 normal{},point{},other{};};
inline Contact contact(Vec2 origin,float angle,const std::array<float,17>& hull,float radius,const World& world,bool groundOnly=false)
{
    using namespace decorTerrain;
    std::array<Vec2,7> points{};
    const unsigned count=unsigned(hull[0]);
    for(unsigned i=0;i<count;++i)points[i]=add(origin,rotate({hull[3+2*i],hull[4+2*i]},angle));
    const Vec2 center=add(origin,rotate(centroid(hull),angle));
    Contact best;
    fixtures(world,area(origin,origin,radius+.02f),[&](const Vec2* vertices,unsigned n,bool filled){
        float winding=0;
        if(filled)for(unsigned j=0;j<n;++j)winding+=cross(vertices[j],vertices[(j+1)%n]);
        for(unsigned edge=0;edge<(filled?n:n-1);++edge){
            const Vec2 a=vertices[edge],delta=sub(vertices[(edge+1)%n],a);
            const float length=std::hypot(delta.x,delta.y);if(length<1e-6f)continue;
            const Vec2 tangent=mul(delta,1/length);
            Vec2 normal{tangent.y,-tangent.x};
            if((filled&&winding<0)||(!filled&&dot(sub(center,a),normal)<0))normal=mul(normal,-1);
            if(groundOnly&&normal.y<=0)continue;
            const float d=dot(sub(center,a),normal);
            if(d < -radius || d>radius+.02f)continue;
            float minimum=1e30f;Vec2 support{},other{};unsigned supports=0;
            auto consider=[&](Vec2 p){
                const float distance=dot(sub(p,a),normal);
                // Source Physics2DSettings m_DefaultContactOffset=.01 permits
                // both ends of an almost level face to share the manifold.
                if(distance<minimum-.01f){minimum=distance;support=other=p;supports=1;}
                else if(std::abs(distance-minimum)<=.01f){
                    minimum=std::min(minimum,distance);
                    if(dot(p,tangent)<dot(support,tangent))support=p;
                    if(dot(p,tangent)>dot(other,tangent))other=p;
                }
            };
            // Clip each hull edge to this finite terrain segment's tangential span.
            // This also admits terrain corners lying between two shard vertices.
            for(unsigned j=0;j<count;++j){
                const Vec2 p=points[j],q=points[(j+1)%count];
                const float u=dot(sub(p,a),tangent),v=dot(sub(q,a),tangent),span=v-u;
                if(std::abs(span)<1e-7f){if(u>=0&&u<=length){consider(p);consider(q);}continue;}
                float lo=(0-u)/span,hi=(length-u)/span;if(lo>hi)std::swap(lo,hi);
                lo=std::max(0.f,lo);hi=std::min(1.f,hi);
                if(lo<=hi){consider(add(p,mul(sub(q,p),lo)));consider(add(p,mul(sub(q,p),hi)));}
            }
            const float depth=std::max(0.f,.005f-minimum);
            if(supports&&minimum<.01f&&minimum>-2*radius&&(!best.hit||depth>best.depth))
                best={true,depth,normal,support,other};
        }
    });
    return best;
}
inline Vec2 separate(Vec2 origin,float angle,const std::array<float,17>& hull,float radius,const World& world)
{
    for(unsigned pass=0;pass<8;++pass){
        const auto c=contact(origin,angle,hull,radius,world);if(!c.hit)break;
        if(c.depth<=0)break;
        origin=decorTerrain::add(origin,decorTerrain::mul(c.normal,c.depth+.00001f));
    }
    return origin;
}
inline void impulse(Vec2& velocity,float& spin,Vec2 arm,Vec2 direction,float amount,float mass,float inertia)
{velocity=decorTerrain::add(velocity,decorTerrain::mul(direction,amount/mass));spin+=cross(arm,direction)*amount/inertia;}
template<class Random>
inline void step(Vec2& origin,Vec2& velocity,float& angle,float& spin,float& sleep,float& touching,
                 float dt,const std::array<float,16>& body,const std::array<float,17>& hull,
                 float gravity,const World& world,Random random)
{
    using namespace decorTerrain;
    if(sleep>=.5f){
        const auto support=contact(add(origin,{0,-.02f}),angle,hull,body[14],world,true);
        if(support.hit&&support.normal.y>0)return;
        sleep=0;
    }
    // Unity source m_Interpolate=0: integrate the COM, not the sprite pivot.
    const Vec2 localCenter=centroid(hull);
    Vec2 center=add(origin,rotate(localCenter,angle));
    const Vec2 oldCenter=center,oldOrigin=origin;const float oldAngle=angle;
    const Vec2 incoming=velocity;
    velocity.y+=gravity*dt;
    velocity=mul(velocity,1/(1+body[7]*dt));spin/=1+body[6]*dt;
    center=add(center,mul(velocity,dt));angle+=spin*dt;
    origin=sub(center,rotate(localCenter,angle));
    Hit sweep;Vec2 sweptLocal{};
    for(unsigned i=0;i<unsigned(hull[0]);++i){
        const Vec2 local{hull[3+2*i],hull[4+2*i]};
        const Vec2 from=add(oldOrigin,rotate(local,oldAngle)),to=add(origin,rotate(local,angle));
        const auto candidate=cast(from,sub(to,from),.005f,world);
        if(candidate.hit&&candidate.fraction<sweep.fraction){sweep=candidate;sweptLocal=local;}
    }
    float remainder=0;
    if(sweep.hit){
        const float fraction=std::min(1.f,sweep.fraction+.0001f);
        remainder=dt*(1-fraction);
        center=add(oldCenter,mul(sub(center,oldCenter),fraction));
        angle=oldAngle+(angle-oldAngle)*fraction;origin=sub(center,rotate(localCenter,angle));
    }
    bool hit=sweep.hit;Vec2 firstNormal=sweep.normal;
    for(unsigned pass=0;pass<8;++pass){
        auto c=contact(origin,angle,hull,body[14],world);
        // Rotation follows an arc while the vertex sweep follows its chord.
        // A clipped sweep must still remove inward contact velocity when the
        // rotated hull lies just outside the static manifold's contact offset.
        if(!c.hit&&pass==0&&sweep.hit){
            const Vec2 point=add(origin,rotate(sweptLocal,angle));
            c={true,0,sweep.normal,point,point};
        }
        if(!c.hit)break;
        if(!hit)firstNormal=c.normal;
        hit=true;
        // Translation-only overlap correction avoids adding rotational energy.
        // Rotation responds to the velocity impulses at the source hull contacts.
        center=add(center,mul(c.normal,c.depth+.00001f));
        origin=sub(center,rotate(localCenter,angle));
        std::array<float,2> normalImpulse{},tangentImpulse{};
        const std::array<Vec2,2> points{c.point,c.other};
        for(unsigned iteration=0;iteration<8;++iteration)for(unsigned index=0;index<2;++index){
        const Vec2 point=points[index];
        const Vec2 arm=sub(point,center),pointVelocity=add(velocity,{-spin*arm.y,spin*arm.x});
        const float normalSpeed=dot(pointVelocity,c.normal),normalLever=cross(arm,c.normal);
        const float next=std::max(0.f,normalImpulse[index]-normalSpeed/(1/body[8]+normalLever*normalLever/body[9]));
        const float force=next-normalImpulse[index];normalImpulse[index]=next;
        impulse(velocity,spin,arm,c.normal,force,body[8],body[9]);
        const Vec2 tangent{-c.normal.y,c.normal.x};
        const float tLever=cross(arm,tangent);
        const float slide=dot(add(velocity,{-spin*arm.y,spin*arm.x}),tangent);
        const float nextFriction=std::clamp(tangentImpulse[index]-slide/(1/body[8]+tLever*tLever/body[9]),
                                          -body[11]*next,body[11]*next);
        const float friction=nextFriction-tangentImpulse[index];tangentImpulse[index]=nextFriction;
        impulse(velocity,spin,arm,tangent,friction,body[8],body[9]);
        }
    }
    if(hit&&touching==0&&std::hypot(incoming.x,incoming.y)>body[13]){
        const float factor=body[12]*random(.8f,1.2f);
        velocity=mul(sub(incoming,mul(firstNormal,2*dot(incoming,firstNormal))),factor);
    }
    // Advance the unconsumed time with the resolved velocity. In particular,
    // a time-zero hit must permit the body's tangential motion and rotation;
    // otherwise gravity accumulates energy in an artificially frozen pose.
    if(remainder>0){
        center=add(center,mul(velocity,remainder));angle+=spin*remainder;
        origin=sub(center,rotate(localCenter,angle));
    }
    origin=separate(origin,angle,hull,body[14],world);
    touching=hit?1.f:0.f;
    // Source Physics2DSettings: .01 units/s, 2 degrees/s, .5 s to sleep.
    if(hit&&std::hypot(velocity.x,velocity.y)<.01f&&std::abs(spin)<.034906585f)sleep+=dt;
    else sleep=0;
    if(sleep>=.5f){velocity={};spin=0;}
}
}
