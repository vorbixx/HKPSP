#include "health_scuttler.hpp"
#include "breakable.hpp"
#include <algorithm>
#include <cmath>

namespace hk3ds {
namespace {
constexpr float pi=3.14159265358979323846f;
// Prefab 436: collider869 and trigger876, scaled by ScuttlerControl.Start.
Rect body(float s){return {-.385f*s,-.505f*s,.385f*s,.385f*s};}
}
Rect HealthScuttler::bounds() const { return boxBounds(position,body(scale)); }
float HealthScuttlers::random(float low,float high){
    random_^=random_<<13;random_^=random_>>17;random_^=random_<<5;
    return low+(high-low)*float(random_>>8)*(1.f/16777216.f);
}
bool HealthScuttlers::bind(const AnimationBank& bank,std::uint32_t seed){
    if(!bank.find("Scuttler Land")||!bank.find("Scuttler Run")||!bank.find("Spawn")||
       !bank.find("Spatter Blue Heal")||!bank.find("Splat")||!bank.find("Health Get"))return false;
    bank_=&bank;seed_=seed?seed:1;
    for(unsigned i=0;i<capacity;++i)
        if(!effects_[i].bind(bank,"Spatter Blue","Spawn","Cocoon Cap",seed_+i))return false;
    reset();return true;
}
void HealthScuttlers::reset(){
    flashAges_.fill(1);
    actors_={};motes_={};splats_={};drawList_={};for(auto& effect:effects_)effect.reset();
    spawned_=false;bounces_=deaths_=heals_=0;random_=seed_;
}
void HealthScuttlers::spawn(Vec2 origin,float depth){
    if(!bank_||spawned_||!std::isfinite(origin.x)||!std::isfinite(origin.y))return;
    spawned_=true;
    for(auto& b:actors_){
        b={};b.phase=ScuttlerPhase::InAir;
        b.position={origin.x+random(-.5f,.5f),origin.y+random(-.5f,.5f)};
        const float speed=random(10,15),angle=random(40,140)*pi/180;
        b.velocity={speed*std::cos(angle),speed*std::sin(angle)};
        b.scale=random(1.35f,1.5f);b.maxSpeed=random(6,9);
        b.history.reset(b.position);b.draw.position=b.position;b.draw.depth=depth;
        b.draw.scale={b.scale,b.scale};b.draw.visible=true;b.draw.player.play(bank_->find("Spawn"));
    }
}
void HealthScuttlers::run(HealthScuttler& b){
    b.phase=ScuttlerPhase::Running;b.timer=0;b.draw.player.play(bank_->find("Scuttler Run"));
}
void HealthScuttlers::fixedUpdate(float dt,const World& world){
    if(!bank_||!std::isfinite(dt)||dt<=0)return;
    for(unsigned i=0;i<capacity;++i){
        auto& b=actors_[i];PresentationPosition::Step step(b.history,b.position);
        effects_[i].fixedUpdate(dt,world);
        if(!b.alive())continue;
        if(b.phase==ScuttlerPhase::Running){
            float distance=0;const float sign=b.velocity.x>=0?1.f:-1.f;
            if(rayTerrain({b.position.x,b.position.y-.06f*b.scale},{sign,0},.385f*b.scale+.1f,world,distance)){
                const float angle=(sign>0?random(110,130):random(50,70))*pi/180;
                b.velocity={5*std::cos(angle),5*std::sin(angle)};
                b.phase=ScuttlerPhase::Bouncing;b.timer=.5f;++bounces_;
            }
        }
        b.velocity.y-=9.81f*.6f*dt;
        const auto contacts=moveBox(b.position,b.velocity,body(b.scale),dt,world);
        if(contacts.ground&&b.phase==ScuttlerPhase::InAir){
            b.phase=ScuttlerPhase::Landing;b.timer=.25f;
            b.draw.player.play(bank_->find("Scuttler Land"),0,true);
        }
        b.draw.position=b.position;
    }
    for(auto& p:motes_){
        PresentationPosition::Step step(p.history,p.position);
        if(p.phase==HealParticle::Phase::Inactive||p.phase==HealParticle::Phase::Flash)continue;
        if(p.phase==HealParticle::Phase::Fall)p.velocity.y-=9.81f*.5f*dt;
        p.position.x+=p.velocity.x*dt;p.position.y+=p.velocity.y*dt;
    }
}
bool HealthScuttlers::hit(unsigned index){
    if(index>=capacity||!bank_)return false;
    auto& b=actors_[index];if(!b.alive()||b.age<.25f)return false;
    b.phase=ScuttlerPhase::Healing;b.timer=1.2f;b.velocity={};b.draw.visible=false;++deaths_;
    effects_[index].burstAt(b.position,b.draw.depth);
    auto& splat=splats_[index];splat.position=b.position;splat.depth=b.draw.depth;
    splat.scale={b.scale,b.scale};splat.visible=true;splat.player.play(bank_->find("Splat"),0,true);
    // Scuttler pool / Spatter Blue Heal FSM: 8..10 projectiles, independent of
    // the 1.2 second health timer. Two reserved ranges prevent cross-owner reuse.
    const unsigned count=std::min(10u,8u+unsigned(random(0,3)));
    for(unsigned i=0;i<count;++i){
        auto& p=motes_[index*10+i];p={};p.owner=index;p.phase=HealParticle::Phase::Fall;
        p.position=b.position;p.history.reset(p.position);
        const float angle=random(30,150)*pi/180,speed=random(15,20),scale=random(1.1f,1.5f);
        p.velocity={std::cos(angle)*speed,std::sin(angle)*speed};p.timer=random(.25f,.45f);
        p.draw.position=p.position;p.draw.depth=-.2f;p.draw.scale={scale,scale};p.draw.visible=true;
        p.draw.player.play(bank_->find("Spatter Blue Heal"));
    }
    return true;
}
void HealthScuttlers::unload(Vitals& vitals){
    for(auto& b:actors_)if(b.phase==ScuttlerPhase::Healing){
        ++vitals.blueHealth;++heals_;b.phase=ScuttlerPhase::Spent;
    }
}
void HealthScuttlers::update(float dt,Hero& hero,const NailBank& nails,DecorEffects* effects){
    if(!bank_||!std::isfinite(dt)||dt<=0)return;
    for(auto& age:flashAges_)age=std::min(1.f,age+dt);
    for(unsigned i=0;i<capacity;++i){
        auto& b=actors_[i];
        if(b.phase==ScuttlerPhase::Spent)continue;
        b.age+=dt;
        if(b.phase==ScuttlerPhase::Healing){
            b.timer-=dt;if(b.timer<=0){++hero.vitals.blueHealth;++heals_;b.phase=ScuttlerPhase::Spent;flashAges_[i]=0;}
            continue;
        }
        b.draw.player.advance(dt);
        if(b.phase==ScuttlerPhase::Landing||b.phase==ScuttlerPhase::Bouncing){
            b.timer-=dt;if(b.timer<=0)run(b);
        }
        if(b.phase==ScuttlerPhase::Running){
            const float side=hero.position.x>=b.position.x?1.f:-1.f;
            b.draw.scale={b.scale*side,b.scale};
            // Source acceleration is per coroutine/render frame. The port uses
            // its existing 60 Hz control clock, independent of display rate.
            b.velocity.x=std::clamp(b.velocity.x-.3f*side,-b.maxSpeed,b.maxSpeed);
        }
        if(hero.nail.active&&hero.nail.hitbox&&
           nailOverlaps(hero,nails.shapes[nailVariant(hero.nail)],b.bounds())&&hit(i)){
            if(effects){effects->enemyKillShake();effects->enemyNailHit(b.position,hero.nail.direction,.9f);}
        }
    }
    for(auto& splat:splats_)if(splat.visible){
        splat.player.advance(dt);if(splat.player.complete)splat.visible=false;
    }
    for(auto& p:motes_){
        if(p.phase==HealParticle::Phase::Inactive)continue;
        if(p.phase==HealParticle::Phase::Flash){
            p.draw.player.advance(dt);
            if(p.draw.player.complete){p.phase=HealParticle::Phase::Inactive;p.draw.visible=false;}
            continue;
        }
        if(actors_[p.owner].phase==ScuttlerPhase::Spent){p.phase=HealParticle::Phase::Inactive;p.draw.visible=false;continue;}
        p.timer-=dt;
        if(p.phase==HealParticle::Phase::Fall&&p.timer<=0)p.phase=HealParticle::Phase::Decelerate;
        if(p.phase==HealParticle::Phase::Decelerate){
            const float speed=std::hypot(p.velocity.x,p.velocity.y);
            // Approximation: authored per-frame FSM acceleration/deceleration
            // advances on the port's 60 Hz control clock.
            const float next=std::max(0.f,speed-.75f*dt*60);
            if(speed>0){p.velocity.x*=next/speed;p.velocity.y*=next/speed;}
            if(next<=.3f){p.phase=HealParticle::Phase::Attract;p.speed=next;}
        }
        if(p.phase==HealParticle::Phase::Attract){
            const Vec2 delta{hero.position.x-p.position.x,hero.position.y-p.position.y};
            const float distance=std::hypot(delta.x,delta.y);
            if(distance<=.6f){
                p.phase=HealParticle::Phase::Flash;p.velocity={};
                const float scale=random(2,2.5f);p.draw.scale={scale,scale};
                // Approximation: shared GeoVisualDraw has no random Z rotation
                // or hero colour flash; retain the original cyan pickup frames.
                p.draw.player.play(bank_->find("Health Get"),0,true);
            }else{
                p.speed=std::min(30.f,p.speed+.85f*dt*60);
                p.velocity={delta.x*p.speed/distance,delta.y*p.speed/distance};
            }
        }
    }
}
float HealthScuttlers::screenFlashStrength(unsigned i) const {
    // Fade Away1186: .25s in/.5s out, easeInOutCirc21. Its serialized
    // Opacity=1 is used; ScuttlerControl's write to absent "Alpha" has no effect.
    const float age=flashAges_[i];
    const float t=std::clamp(age<.25f?age/.25f:1-(age-.25f)/.5f,0.f,1.f);
    return t<.5f?(1-std::sqrt(std::max(0.f,1-4*t*t)))*.5f:
        (std::sqrt(std::max(0.f,1-4*(1-t)*(1-t)))+1)*.5f;
}
unsigned HealthScuttlers::activeMotes() const {
    unsigned n=0;for(const auto& p:motes_)if(p.phase!=HealParticle::Phase::Inactive)++n;return n;
}
const std::array<GeoVisualDraw,HealthScuttlers::healCapacity+HealthScuttlers::capacity>&
HealthScuttlers::draws(PresentationTime time) const {
    for(unsigned i=0;i<healCapacity;++i){
        drawList_[i]=motes_[i].draw;
        drawList_[i].position=motes_[i].history.sample(motes_[i].position,time);
    }
    for(unsigned i=0;i<capacity;++i)drawList_[healCapacity+i]=splats_[i];
    return drawList_;
}
}
