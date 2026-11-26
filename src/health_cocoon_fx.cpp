#include "health_cocoon_fx.hpp"
#include "cosmetic_terrain.hpp"
#include <algorithm>
#include <cmath>

namespace hk3ds {
namespace {
constexpr float pi=3.14159265358979323846f;
constexpr float contactSkin=.025f;
// Visual damping estimate only; source does not establish Blue Glob's material
// restitution (the separate cocoon-cap script's .4 factor is not transferable).
constexpr float globRestitution=.4f;
struct Hit { float distance=0; Vec2 normal{}; };
using namespace cosmetic_terrain;
bool sweepSolid(Vec2 p,Vec2 d,float length,const World& w,Hit& out) {
    bool found=false;out.distance=length;
    auto accept=[&](float t,Vec2 n) {
        if(t<=1e-5f||t>out.distance||dot(d,n)>=-1e-6f)return;
        out.distance=t;out.normal=n;found=true;
    };
    for(std::size_t i=0;i<w.shapeCount;++i) {
        const auto& s=w.shapes[i];
        if(!s.enabled||(s.flags&(1u|TerrainShape::queryOnly))||!w.vertices||s.count<2)continue;
        const Rect b=shapeBounds(s,w);
        if(b.right<std::min(p.x,p.x+d.x*length)||b.left>std::max(p.x,p.x+d.x*length)||
           b.top<std::min(p.y,p.y+d.y*length)||b.bottom>std::max(p.y,p.y+d.y*length))continue;
        bool ccw=true;
        if(s.kind==0){float area=0;for(std::size_t j=0;j<s.count;++j)area+=cross(w.vertices[s.first+j],w.vertices[s.first+(j+1)%s.count]);ccw=area>=0;}
        const std::size_t edges=s.kind==0?s.count:s.count-1;
        for(std::size_t j=0;j<edges;++j) {
            const Vec2 a=w.vertices[s.first+j],b=w.vertices[s.first+(j+1)%s.count],e=subtract(b,a);
            const float den=cross(d,e);if(std::abs(den)<1e-9f)continue;
            const Vec2 ap=subtract(a,p);const float t=cross(ap,e)/den,u=cross(ap,d)/den;
            if(t<0||u<0||u>1)continue;
            Vec2 n=ccw?Vec2{e.y,-e.x}:Vec2{-e.y,e.x};const float l=std::sqrt(dot(n,n));if(l<=1e-9f)continue;
            n={n.x/l,n.y/l};if(dot(d,n)>0)n={-n.x,-n.y};accept(t,n);
        }
    }
    for(std::size_t i=0;i<w.count;++i) {
        const Rect b=w.solids[i].bounds;
        float near=0,far=length;Vec2 n{};
        auto axis=[&](float origin,float speed,float low,float high,Vec2 lowNormal,Vec2 highNormal) {
            if(std::abs(speed)<1e-10f)return origin>=low&&origin<=high;
            float a=(low-origin)/speed,z=(high-origin)/speed;Vec2 candidate=speed>0?lowNormal:highNormal;
            if(a>z)std::swap(a,z);
            if(a>near){near=a;n=candidate;}
            far=std::min(far,z);return near<=far;
        };
        if(axis(p.x,d.x,b.left,b.right,{-1,0},{1,0})&&axis(p.y,d.y,b.bottom,b.top,{0,-1},{0,1})&&near>=0)accept(near,n);
    }
    return found;
}
}

bool HealthCocoonFx::bind(const AnimationBank& bank,const char* spatterClip,
                          const char* globClip,const char* capClip,std::uint32_t seed) {
    if(!spatterClip||!globClip||!capClip)return false;
    const Clip* spatter=bank.find(spatterClip);
    const Clip* glob=bank.find(globClip);
    const Clip* cap=bank.find(capClip);
    if(!spatter||!glob||!cap||spatter->fps<=0||glob->fps<=0||cap->fps<=0||
       !spatter->count||!glob->count||!cap->count)return false;
    bank_=&bank;spatterClip_=spatter;globClip_=glob;capClip_=cap;
    spatterImpactClip_=bank.find("Spatter Blue Impact");
    globLandClip_=bank.find("Glob Land");globWobbleClip_=bank.find("Glob Wobble");
    seed_=seed?seed:1;reset();return true;
}

bool HealthCocoonFxConfig::read(std::FILE* file) {
    auto fling=[&](Fling& f) {
        std::uint32_t counts[2];float v[6];
        if(std::fread(counts,4,2,file)!=2||std::fread(v,4,6,file)!=6)return false;
        f={counts[0],counts[1],{v[0],v[1]},v[2],v[3],v[4],v[5]};return true;
    };
    float v[22];
    if(!fling(spatter)||!fling(glob)||std::fread(v,4,22,file)!=22)return false;
    capForce=v[0];capMass=v[1];capGravity=v[2];globGravity=v[3];spatterGravity=v[4];
    bounceFactor=v[5];bounceThreshold=v[6];spinFactor=v[7];angularDamping=v[8];
    spatterScaleMin=v[9];spatterScaleMax=v[10];splashScaleMin=v[11];splashScaleMax=v[12];
    globScaleMin=v[13];globScaleMax=v[14];capOffset={v[15],v[16]};
    capScale={v[17],v[18]};capDepth=v[19];gravity=v[20];capInertia=v[21];return valid();
}

bool HealthCocoonFxConfig::valid() const {
    auto fling=[](const Fling& f) {
        return f.minAmount<=f.maxAmount&&std::isfinite(f.variation.x)&&std::isfinite(f.variation.y)&&
            f.variation.x>=0&&f.variation.y>=0&&std::isfinite(f.minSpeed)&&std::isfinite(f.maxSpeed)&&
            f.minSpeed>=0&&f.maxSpeed>=f.minSpeed&&std::isfinite(f.minAngle)&&
            std::isfinite(f.maxAngle)&&f.maxAngle>=f.minAngle;
    };
    const float values[]={capForce,capMass,capGravity,globGravity,spatterGravity,bounceFactor,
        bounceThreshold,spinFactor,angularDamping,spatterScaleMin,spatterScaleMax,splashScaleMin,
        splashScaleMax,globScaleMin,globScaleMax,capOffset.x,capOffset.y,capScale.x,capScale.y,capDepth,
        gravity,capInertia};
    for(float value:values)if(!std::isfinite(value))return false;
    // The flight/impact slots are partitioned by source prefab, never by live count.
    return fling(spatter)&&fling(glob)&&spatter.minAmount==50&&spatter.maxAmount==50&&glob.maxAmount<=10&&
        capForce>=0&&capMass>0&&capGravity>=0&&globGravity>=0&&spatterGravity>=0&&
        bounceFactor>=0&&bounceFactor<=1&&bounceThreshold>=0&&angularDamping>=0&&
        spatterScaleMin>0&&spatterScaleMax>=spatterScaleMin&&splashScaleMin>0&&
        splashScaleMax>=splashScaleMin&&globScaleMin>0&&globScaleMax>=globScaleMin&&
        capScale.x>0&&capScale.y>0&&gravity<0&&capInertia>0;
}

bool HealthCocoonFx::configure(const HealthCocoonFxConfig& config) {
    if(!config.valid())return false;
    config_=config;return true;
}

void HealthCocoonFx::reset() {
    for(auto& p:particles_)p=Particle{};
    drawList_={};activeCount_=0;random_=seed_;capActive_=false;
}

float HealthCocoonFx::random(float low,float high) {
    random_^=random_<<13;random_^=random_>>17;random_^=random_<<5;
    const float u=static_cast<float>(random_>>8)*(1.0f/16777216.0f);
    return low+(high-low)*u;
}

unsigned HealthCocoonFx::randomCount(unsigned low,unsigned high) {
    if(high<=low)return low;
    // HealthCocoon.FlingObjects calls Random.Range(minAmount,maxAmount+1).
    const unsigned span=high-low+1;
    return low+std::min(span-1,static_cast<unsigned>(random(0.0f,float(span))));
}

void HealthCocoonFx::emit(const Clip* clip,unsigned amount,Vec2 origin,
                          Vec2 variation,float minSpeed,float maxSpeed,
                          float angleLow,float angleHigh) {
    if(!clip||!bank_)return;
    for(unsigned n=0;n<amount&&activeCount_<particleCapacity;++n) {
        auto it=std::find_if(particles_.begin(),particles_.begin()+particleCapacity,
                             [](const Particle& p){return !p.active;});
        if(it==particles_.begin()+particleCapacity)break;
        auto& p=*it;p=Particle{};p.active=true;
        p.position={origin.x+random(-variation.x,variation.x),
                    origin.y+random(-variation.y,variation.y)};
        const float speed=random(minSpeed,maxSpeed);
        const float angle=random(angleLow,angleHigh)*pi/180.0f;
        p.velocity={std::cos(angle)*speed,std::sin(angle)*speed};
        // Spatter Blue uses frame 0 for flight, then frames 1..6 for a 30 FPS
        // splash after collision or its 3 s timeout. Blue Glob stays until broken.
        p.lifetime=clip==spatterClip_?3.25f:1000000.0f;
        if(clip==spatterClip_)p.scaleModifier=random(config_.spatterScaleMin,config_.spatterScaleMax);
        else p.scaleModifier=random(config_.globScaleMin,config_.globScaleMax);
        p.draw.position=p.position;p.draw.depth=0;
        p.draw.scale={p.scaleModifier,p.scaleModifier};
        p.draw.visible=true;p.draw.player.play(clip,0,true);
        if(clip==spatterClip_)p.draw.rotation=angle;
        p.history.reset(p.position);++activeCount_;
    }
}

void HealthCocoonFx::burstAt(Vec2 origin,float depth) {
    reset();depth_=depth;
    // ScuttlerControl.Hit: 12..18 blue blood projectiles, 4..22 speed, 30..150 degrees.
    emit(spatterClip_,randomCount(12,18),origin,{},4,22,30,150);
    for(auto& p:particles_)if(p.active)p.draw.depth=depth;
}

void HealthCocoonFx::breakAt(Vec2 origin,float depth,Vec2 nailPosition) {
    reset();
    for(const auto* f:{&config_.spatter,&config_.glob})
        emit(f==&config_.spatter?spatterClip_:globClip_,randomCount(f->minAmount,f->maxAmount),
             origin,f->variation,f->minSpeed,f->maxSpeed,f->minAngle,f->maxAngle);
    if(std::isfinite(depth))depth_=depth;
    for(unsigned i=0;i<particleCapacity;++i)
        if(particles_[i].active)particles_[i].draw.depth=depth_;
    auto& cap=particles_[particleCapacity];cap=Particle{};cap.active=true;
    cap.position=hasCapTransform_?capPosition_:Vec2{origin.x+config_.capOffset.x,origin.y+config_.capOffset.y};
    cap.depth=depth_+config_.capDepth;cap.lifetime=1000000.0f;
    const Vec2 away=subtract(origin,nailPosition);
    const float distance=std::sqrt(dot(away,away));
    // HealthCocoon normalizes root-minus-collider-transform, then applies an impulse.
    // Unity's zero vector normalizes to zero; never invent a direction in that case.
    if(std::isfinite(distance)&&distance>1e-5f) {
        const float impulse=config_.capForce/(config_.capMass*distance);
        cap.velocity={away.x*impulse,away.y*impulse};
    }
    cap.draw.position=cap.position;cap.draw.depth=cap.depth;
    cap.draw.scale=hasCapTransform_?capScale_:config_.capScale;
    cap.draw.rotation=static_cast<unsigned>(random(0,360))*pi/180.0f;
    cap.draw.visible=true;cap.draw.player.play(capClip_,0,true);
    cap.history.reset(cap.position);capActive_=true;++activeCount_;
}

void HealthCocoonFx::breakCallback(void* context,Vec2 origin,Strike,Vec2 nailPosition) {
    if(context) {
        auto& fx=*static_cast<HealthCocoonFx*>(context);
        fx.breakAt(origin,fx.depth_,nailPosition);
    }
}

void HealthCocoonFx::fixedUpdate(float seconds,const World& world) {
    if(!std::isfinite(seconds)||seconds<=0)return;
    for(unsigned i=0;i<capacity;++i) {
        auto& p=particles_[i];
        PresentationPosition::Step step(p.history,p.position);
        if(!p.active)continue;
        p.age+=seconds;
        if(i<50&&p.landed) {
            p.phaseElapsed+=seconds;p.draw.player.advance(seconds);
            const float duration=spatterImpactClip_?
                static_cast<float>(spatterImpactClip_->count)/spatterImpactClip_->fps:.2f;
            if(p.phaseElapsed>=duration) {
                p.active=false;p.draw.visible=false;--activeCount_;
            }
            continue;
        }
        if(i<50&&p.age>=3.0f) {
            p.landed=true;p.phaseElapsed=0;p.velocity={0,0};
            p.scaleModifier=random(config_.splashScaleMin,config_.splashScaleMax);
            p.draw.scale={p.scaleModifier,p.scaleModifier};
            if(spatterImpactClip_)p.draw.player.play(spatterImpactClip_,0,true);
            else p.draw.player.play(spatterClip_,0,true);
            continue;
        }
        if(i>=50&&i<particleCapacity&&p.landed) {
            p.phaseElapsed+=seconds;
            if(p.draw.player.clip==globLandClip_&&globWobbleClip_&&
               p.phaseElapsed>=static_cast<float>(globLandClip_->count)/globLandClip_->fps) {
                p.draw.player.play(globWobbleClip_,0,true);p.phaseElapsed=0;
            }
            p.draw.player.advance(seconds);continue;
        }
        // Scene geometry can overlap the effect origin. Resolve that overlap
        // before a ray sweep, which otherwise has no entry hit to report.
        if(!p.collisionInitialized) {
            p.collisionInitialized=true;
            if(depenetrate(p.position,world)) {
                p.draw.position=p.position;
                p.history.reset(p.position);
            }
        }
        const bool cap=i==particleCapacity;
        const float gravityScale=cap?config_.capGravity:(i<50?config_.spatterGravity:config_.globGravity);
        p.velocity.y+=config_.gravity*gravityScale*seconds;
        if(cap) {
            // SpinSelf applies one torque on its second FixedUpdate.
            if(p.physicsSteps++==1)p.angularVelocity+=p.velocity.x*config_.spinFactor*seconds/config_.capInertia;
            p.angularVelocity/=1+config_.angularDamping*seconds;
            p.draw.rotation=std::remainder(p.draw.rotation+p.angularVelocity*seconds,2*pi);
        }
        const Vec2 next{p.position.x+p.velocity.x*seconds,
                        p.position.y+p.velocity.y*seconds};
        const Vec2 delta{next.x-p.position.x,next.y-p.position.y};
        const float length=std::sqrt(delta.x*delta.x+delta.y*delta.y);
        Hit hit{};
        if(length>0&&sweepSolid(p.position,{delta.x/length,delta.y/length},length,world,hit)) {
            const float t=std::max(0.0f,hit.distance-contactSkin)/length;
            p.position={p.position.x+delta.x*t,p.position.y+delta.y*t};
            if(i<50) {
                p.velocity={0,0};
                p.landed=true;p.phaseElapsed=0;p.scaleModifier=random(config_.splashScaleMin,config_.splashScaleMax);
                p.draw.scale={p.scaleModifier,p.scaleModifier};
                p.draw.rotation=std::atan2(hit.normal.y,hit.normal.x)-pi/2;
                if(spatterImpactClip_)p.draw.player.play(spatterImpactClip_,0,true);
                else p.draw.player.play(spatterClip_,0,true);
            } else if(i<particleCapacity&&hit.normal.y>.55f) {
                p.velocity={0,0};
                p.landed=true;p.phaseElapsed=0;
                if(globLandClip_)p.draw.player.play(globLandClip_,0,true);
                else if(globWobbleClip_)p.draw.player.play(globWobbleClip_,0,true);
            } else if(cap) {
                // ObjectBounce reflects the whole velocity and scales its speed,
                // rather than applying restitution only along the contact normal.
                const float speed=std::sqrt(dot(p.velocity,p.velocity));
                const float inward=dot(p.velocity,hit.normal);
                if(inward<0) {
                    if(speed>config_.bounceThreshold) {
                        const float damping=config_.bounceFactor*random(.8f,1.2f);
                        p.velocity={(p.velocity.x-2*inward*hit.normal.x)*damping,
                                    (p.velocity.y-2*inward*hit.normal.y)*damping};
                    } else {
                        p.velocity.x-=inward*hit.normal.x;p.velocity.y-=inward*hit.normal.y;
                    }
                }
            } else {
                const float inward=dot(p.velocity,hit.normal);
                if(inward<0) {
                    p.velocity.x-=(1.0f+globRestitution)*inward*hit.normal.x;
                    p.velocity.y-=(1.0f+globRestitution)*inward*hit.normal.y;
                }
            }
        } else p.position=next;
        if(i<50) {
            const float speed=std::sqrt(p.velocity.x*p.velocity.x+p.velocity.y*p.velocity.y);
            p.draw.scale={(1.0f+speed*.014f)*p.scaleModifier,
                          (1.0f-speed*.014f)*p.scaleModifier};
            if(!p.landed)p.draw.rotation=std::atan2(p.velocity.y,p.velocity.x);
        } else if(i<particleCapacity)p.draw.scale={p.scaleModifier,p.scaleModifier};
        p.draw.position=p.position;p.draw.player.advance(seconds);
    }
}

const std::array<GeoVisualDraw,HealthCocoonFx::capacity>&
HealthCocoonFx::draws(PresentationTime time) const {
    for(unsigned i=0;i<capacity;++i) {
        drawList_[i]=particles_[i].draw;
        if(particles_[i].active)
            drawList_[i].position=particles_[i].history.sample(particles_[i].position,time);
    }
    return drawList_;
}

bool HealthCocoonFx::invariant() const {
    unsigned active=0;
    for(unsigned i=0;i<capacity;++i) {
        const auto& p=particles_[i];
        if(i==particleCapacity&&p.active!=capActive_)return false;
        if(p.active) {
            ++active;
            if(!p.draw.visible||!std::isfinite(p.position.x)||!std::isfinite(p.position.y)||
               !std::isfinite(p.velocity.x)||!std::isfinite(p.velocity.y)||!std::isfinite(p.draw.rotation)||
               !std::isfinite(p.age)||p.age<0||!std::isfinite(p.lifetime)||p.lifetime<=0||
               (i<particleCapacity&&p.age>=p.lifetime))return false;
        } else if(p.draw.visible)return false;
    }
    return active==activeCount_&&activeCount_<=capacity;
}

}
