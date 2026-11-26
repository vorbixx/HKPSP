#include "elder_baldur.hpp"
#include <algorithm>
#include <cmath>

namespace hk3ds {
bool ElderBaldur::bind(CombatObject& object,const AnimationBank& bank,Vec2 p,float scale,EnemyRandom random,CombatObject* rollerObject){
    if(!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(scale)||scale==0||(!random.unit&&!random.sample))return false;
    for(const char* clip:{"Closed","Open","Idle","Close1","Close2","Hit","Shoot Antic","Shoot CD"})
        if(!bank.find(clip))return false;
    for(const char* clip:{"Goop/Idle","Goop/Impact","Roller/Start","Roller/Stop","Roller/Idle","Roller/Roll","Roller/Death Air"})
        if(!bank.find(clip))return false;
    *this={};object_=&object;bank_=&bank;position=p;scaleX=scale;random_=random;
    rollerObject_=rollerObject;
    if(rollerObject_){*rollerObject_={};rollerObject_->colliderEnabled=false;}
    object={};object.hasHealth=true;object.health.hp=60;object.health.invincible=true;
    object.bounds=boxBounds(position,body,scaleX);object.name="Blocker";
    object.receiver=this;object.onHit=receive;enter(Phase::Dormant,"Closed");return true;
}
bool ElderBaldur::inRange(const Hero& hero,bool attack)const{
    // Apply child scale and translation before the mirrored parent transform.
    const float x=attack?-.40833753f:-.33226106f,y=attack?2.218812f:.9850731f;
    const float w=attack?1.8166769f:.7181119f,h=attack?7.5337644f:5.936685f;
    const float sx=attack?4.95f:21.14f,sy=attack?1.95f:1.9f,ty=attack?-.48f:.31f;
    const Rect local{(x-w/2)*sx,ty+(y-h/2)*sy,(x+w/2)*sx,ty+(y+h/2)*sy};
    const auto a=boxBounds(position,local,scaleX),b=bodyBounds(hero.position);
    return a.left<b.right&&a.right>b.left&&a.bottom<b.top&&a.top>b.bottom;
}
void ElderBaldur::enter(Phase next,const char* clip){
    phase=next;animation.play(bank_->require(clip),0,true);
    object_->health.invincible=next==Phase::Dormant||next==Phase::Close1||next==Phase::Close2||next==Phase::Closed;
    if(next==Phase::Idle)timer_=random_.range(.8f,1.2f);
}
void ElderBaldur::receive(void* context,const NailHit&,HitOutcome outcome){
    auto& self=*static_cast<ElderBaldur*>(context);
    if(outcome==HitOutcome::Killed){
        self.phase=Phase::Dead;self.object_->colliderEnabled=false;self.death_=true;
        // Parent Destroy FSM deactivates Blocker immediately on KILLED.
        self.animation={};
    }else if(outcome==HitOutcome::Damaged)self.enter(Phase::Hit,"Hit");
}
void ElderBaldur::fixedUpdate(float dt,Hero& hero,const World& world){
    if(dt<=0||!std::isfinite(dt))return;
    attacks(dt,hero,world);
    if(!alive())return;
    const bool close=inRange(hero,true);
    switch(phase){
    case Phase::Dormant:if(inRange(hero,false))enter(Phase::Open,"Open");break;
    case Phase::Open:if(animation.complete)enter(Phase::Idle,"Idle");break;
    case Phase::Idle:
        if(close){sleeping_=false;enter(Phase::Close1,"Close1");}
        else if(!inRange(hero,false)){sleeping_=true;enter(Phase::Close1,"Close1");}
        else if((timer_-=dt)<=0){
            // Can Roller? falls back to goop without Fireball or while one is alive.
            spawnRoller_=random_.range(0,1)>=.5f&&hero.abilities.spiritLevel()>0&&rollerObject_&&roller.phase==RollerPhase::Gone;
            enter(Phase::ShootAntic,"Shoot Antic");
        }
        break;
    case Phase::Close1:if(animation.complete)enter(Phase::Close2,"Close2");break;
    case Phase::Close2:if(animation.complete)enter(sleeping_?Phase::Dormant:Phase::Closed,"Closed");break;
    case Phase::Closed:if(!close)enter(Phase::Open,"Open");break;
    case Phase::Hit:
    case Phase::ShootCooldown:
        if(close){sleeping_=false;enter(Phase::Close1,"Close1");}
        else if(animation.complete)enter(Phase::Idle,"Idle");
        break;
    case Phase::ShootAntic:
        if(animation.complete){fire();enter(Phase::ShootCooldown,"Shoot CD");}
        break;
    case Phase::Dead:break;
    }
}
void ElderBaldur::rollerEnter(RollerPhase phase,const char* name){
    roller.phase=phase;roller.animation.play(bank_->require(name),0,true);
}
void ElderBaldur::fire(){
    // This instance has Facing Right=true and source scaleX=-1.
    const float facing=scaleX<0?1.f:-1.f;
    const Vec2 origin{position.x+2.76f*facing,position.y+.78f};
    // Direction states author asymmetric left [-1,-15] and right [3,15] ranges.
    const Vec2 velocity{facing<0?random_.range(-15,-1):random_.range(3,15),spawnRoller_?0.f:20.f};
    if(spawnRoller_){
        roller={};roller.position=origin;roller.velocity=velocity;roller.scaleX=-facing;roller.presentation.reset(origin);
        *rollerObject_={};rollerObject_->name="Spawn Roller v2";rollerObject_->hasHealth=true;rollerObject_->health.hp=15;
        rollerObject_->receiver=this;rollerObject_->onHit=receiveRoller;
        rollerObject_->bounds=boxBounds(origin,{-.5625f,-.65625f,.53125f,.4375f},roller.scaleX);
        rollerEnter(RollerPhase::Air,"Roller/Roll");return;
    }
    for(auto& bullet:bullets)if(!bullet.active){
        bullet={};bullet.active=true;bullet.position=origin;bullet.velocity=velocity;
        bullet.scale=random_.range(1.15f,1.45f);bullet.presentation.reset(origin);
        bullet.animation.play(bank_->require("Goop/Idle"),0,true);return;
    }
}
void ElderBaldur::receiveRoller(void* context,const NailHit& hit,HitOutcome outcome){
    auto& self=*static_cast<ElderBaldur*>(context);
    if(outcome==HitOutcome::Killed){self.rollerObject_->colliderEnabled=false;self.rollerEnter(RollerPhase::Dead,"Roller/Death Air");self.roller.timer=.25f;}
    if(outcome==HitOutcome::Damaged||outcome==HitOutcome::Killed)
        self.roller.velocity.x=(hit.direction==Strike::Right?25.f:hit.direction==Strike::Left?-25.f:0.f)*hit.recoilMultiplier;
}
void ElderBaldur::attacks(float dt,Hero& hero,const World& world){
    const auto touching=[&](Rect a){const auto b=bodyBounds(hero.position);return a.left<b.right&&a.right>b.left&&a.bottom<b.top&&a.top>b.bottom;};
    for(auto& b:bullets)if(b.active){
        PresentationPosition::Step history(b.presentation,b.position);
        if(b.impact){if(b.animation.complete)b.active=false;continue;}
        b.velocity.y-=36.f*dt; // Physics2D gravity -60 * prefab gravityScale .6.
        b.rotation=std::atan2(b.velocity.y,b.velocity.x)*57.2957795f;
        const Rect body{-.3125f*b.scale,-.28125f*b.scale,.328125f*b.scale,.28125f*b.scale};
        auto v=b.velocity;const auto contact=moveBox(b.position,v,body,dt,world);
        const bool heroHit=touching(boxBounds(b.position,body));
        if(heroHit)hero.takeDamage(1,hero.position.x<b.position.x?1:-1);
        if(heroHit||contact.ground||contact.roof||contact.left||contact.right){
            b.impact=true;b.animation.play(bank_->require("Goop/Impact"),0,true);
        }
        // Native pool guard for shots which leave all resident collision geometry.
        if(b.position.y<position.y-100)b.active=false;
    }
    if(roller.phase==RollerPhase::Gone)return;
    PresentationPosition::Step history(roller.presentation,roller.position);
    const bool dead=roller.phase==RollerPhase::Dead;
    if(roller.phase==RollerPhase::Roll){
        const float facing=-roller.scaleX;
        roller.velocity.x=std::clamp(roller.velocity.x+facing*.45f,-14.f,14.f);
        if((roller.timer-=dt)<=0){roller.velocity.x=0;rollerEnter(RollerPhase::Stop,"Roller/Stop");}
    }else if(roller.phase==RollerPhase::Stop&&roller.animation.complete){roller.timer=.5f;rollerEnter(RollerPhase::Rest,"Roller/Idle");}
    else if(roller.phase==RollerPhase::Rest&&(roller.timer-=dt)<=0){
        roller.scaleX=hero.position.x<roller.position.x?1.f:-1.f;rollerEnter(RollerPhase::Start,"Roller/Start");
    }else if(roller.phase==RollerPhase::Start&&roller.animation.complete){roller.timer=random_.range(2,3);rollerEnter(RollerPhase::Roll,"Roller/Roll");}
    roller.velocity.y-=48.f*dt; // Source gravityScale .8.
    const Rect body{-.5625f,-.65625f,.53125f,.4375f};
    const auto contact=moveBox(roller.position,roller.velocity,boxBounds({},body,roller.scaleX),dt,world);
    if(dead){if((roller.timer-=dt)<=0)roller.phase=RollerPhase::Gone;return;}
    if(contact.ground&&roller.phase==RollerPhase::Air){roller.timer=random_.range(2,3);rollerEnter(RollerPhase::Roll,"Roller/Roll");}
    if(contact.left||contact.right){roller.scaleX=-roller.scaleX;roller.velocity={};rollerEnter(RollerPhase::Air,"Roller/Roll");}
    rollerObject_->bounds=boxBounds(roller.position,body,roller.scaleX);
    if(touching(rollerObject_->bounds))hero.takeDamage(1,hero.position.x<roller.position.x?1:-1);
    if(roller.position.y<position.y-100){rollerObject_->colliderEnabled=false;roller.phase=RollerPhase::Gone;}
}
void ElderBaldur::update(float dt){
    if(dt<0||!std::isfinite(dt))return;
    if(alive())animation.advance(dt);
    for(auto& b:bullets)if(b.active)b.animation.advance(dt);
    if(roller.phase!=RollerPhase::Gone)roller.animation.advance(dt);
}
}
