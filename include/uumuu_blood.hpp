#pragma once
#include "presentation.hpp"
#include "uumuu_blood_data.hpp"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace hk3ds {
// Scene-local presentation RNG: blood must never change the boss/Quirrel RNG.
class UumuuBlood {
    friend struct SimulationStateHash;
public:
    static constexpr unsigned capacity=512;
    struct Particle {
        Vec2 position{},velocity{};
        PresentationPosition presentation;
        float age=0,life=0,size=0,force=0;
        unsigned tile=0;
        bool leftSpawnSurface=false;
    };
    struct Draw {Vec2 position{};float width=0,height=0,rotation=0,alpha=0;unsigned tile=0;};
    void reset(){particles_={};next_=0;seed_=14766;}
    unsigned count()const{unsigned n=0;for(const auto& p:particles_)if(p.life>0)++n;return n;}
    void emitSource(Vec2 at,const float* config){
        at.x+=config[0];at.y+=config[1];
        const unsigned count=std::min(unsigned(config[4]),unsigned(random(config[3],config[4]+1)));
        emit(at,count,config[5],config[6],config[7],config[8]);
    }
    void emit(Vec2 at,unsigned count,float speedLo,float speedHi,float angleLo,float angleHi,
              float spawnRadius=uumuu_blood_data::shapeRadius*uumuu_blood_data::scale){
        namespace d=uumuu_blood_data;
        if(!std::isfinite(at.x)||!std::isfinite(at.y))return;
        for(unsigned i=0;i<std::min(count,capacity);++i){
            auto& p=particles_[next_++%capacity];p={};
            const float angle=random(angleLo,angleHi)*.01745329252f,speed=random(speedLo,speedHi)*1.2f;
            // GlobalPrefabDefaults changes the source circle's arc/rotation,
            // retaining its filled radius. Point emission can trap an entire
            // corpse burst inside a platform instead of letting it rise above.
            const float radius=std::sqrt(random(0,1))*spawnRadius;
            p.position={at.x+std::cos(angle)*radius,at.y+std::sin(angle)*radius};
            p.presentation.reset(p.position);
            p.velocity={std::cos(angle)*speed,std::sin(angle)*speed+random(d::velocityY[0],d::velocityY[1])};
            p.life=random(d::life[0],d::life[1]);p.size=random(d::size[0],d::size[1])*d::scale;
            p.force=random(d::forceY[0],d::forceY[1]);p.tile=std::min(2u,unsigned(random(0,3)));
        }
    }
    void fixedUpdate(float dt,const World& world){
        if(!std::isfinite(dt)||dt<=0)return;
        namespace d=uumuu_blood_data;
        for(auto& p:particles_)if(p.life>0){
            PresentationPosition::Step step(p.presentation,p.position);
            p.age+=dt;if(p.age>=p.life){p.life=0;continue;}
            p.velocity.y+=p.force*dt;
            const float speed=std::hypot(p.velocity.x,p.velocity.y);
            if(speed>d::speedLimit&&d::speedLimit>0){p.velocity.x*=d::speedLimit/speed;p.velocity.y*=d::speedLimit/speed;}
            const Vec2 delta{p.velocity.x*dt,p.velocity.y*dt};const float length=std::hypot(delta.x,delta.y);
            float distance=0;
            // Source blood dies on Terrain; collision sub-emitter is omitted.
            if(length>0){
                const bool hit=rayTerrain(p.position,{delta.x/length,delta.y/length},length,world,distance);
                // A corpse can lie exactly on a platform. Clear its initial
                // zero-distance contact once; later terrain entry still kills.
                if(hit&&(distance>0||p.leftSpawnSurface)){p.life=0;continue;}
                if(!hit)p.leftSpawnSurface=true;
            }
            p.position.x+=delta.x;p.position.y+=delta.y;
        }
    }
    unsigned capture(std::array<Draw,capacity>& draws,PresentationTime time)const{
        namespace d=uumuu_blood_data;
        unsigned n=0;
        for(const auto& p:particles_)if(p.life>0){
            auto& out=draws[n++];out.position=p.presentation.sample(p.position,time);
            const float t=p.age/p.life,phase=std::clamp(t*7,0.f,7.f);const unsigned a=unsigned(phase),b=std::min(a+1,7u);
            const auto sample=[&](const float* curve){return curve[a]+(curve[b]-curve[a])*(phase-a);};
            out.width=p.size*sample(d::sizeCurve);
            out.height=out.width*d::lengthScale+std::hypot(p.velocity.x,p.velocity.y)*d::velocityScale;
            out.rotation=std::atan2(p.velocity.y,p.velocity.x)-1.57079632679f;
            const float pivot=out.height*d::pivotY;
            out.position.x+=pivot*std::sin(out.rotation);out.position.y-=pivot*std::cos(out.rotation);
            out.alpha=sample(d::alphaCurve);out.tile=p.tile;
        }
        return n;
    }
private:
    float random(float lo,float hi){seed_^=seed_<<13;seed_^=seed_>>17;seed_^=seed_<<5;return lo+(hi-lo)*(seed_>>8)*(1.f/16777216.f);}
    std::array<Particle,capacity> particles_{};
    unsigned next_=0;std::uint32_t seed_=14766;
};
}
