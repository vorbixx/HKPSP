#pragma once
#include "combat.hpp"
#include "hero_tuning.hpp"
#include <array>
#include <algorithm>

namespace hk3ds {
struct SpiritProjectile {
    PresentationPosition presentation;
    Vec2 position{};
    float life=0,age=0;
    int direction=1;
    int damage=tuning::spiritDamage;
    bool upgraded=false;
    Vec2 spriteScale{1.2f,1.2f};
    float speed=tuning::spiritSpeed,hitTime=tuning::spiritLife;
    Rect hitbox{-1.1f,-.65f,1.1f,.65f};
    std::array<bool,CombatScene::capacity> hit{};
    std::array<bool,CombatScene::capacity> encounterHit{},roomHit{};
    bool active() const{return life>0;}
    Vec2 renderPosition(PresentationTime time) const{return presentation.sample(position,time);}
    const char* animation() const{return upgraded?"Shade Soul":(life<.12f?"Spirit End":"Spirit");}
    float opacity() const{return upgraded?1.f:std::min(1.f,life/.12f);}
    Vec2 renderScale() const{
        if(!upgraded)return spriteScale;
        // Fireball Control22074: collider off, ScaleBy(.75,.25) over .075s.
        // approximation: linear tween; the original iTween easing is not modeled.
        const float shrink=std::clamp((age-hitTime)/(tuning::shadeSoulLife-tuning::shadeSoulHitTime),0.f,1.f);
        return {spriteScale.x*(1.f-.25f*shrink),spriteScale.y*(1.f-.75f*shrink)};
    }
};
class SpiritProjectiles {
    friend struct SimulationStateHash;
public:
    static constexpr unsigned capacity=4;
    std::array<SpiritProjectile,capacity> actors{};
    void observe(const Hero& hero){
        if(serial_==hero.spellSerial)return;
        serial_=hero.spellSerial;
        // Soul capacity and recovery bound the number of simultaneous spirits.
        for(auto& p:actors)if(!p.active()){
            p={};p.direction=hero.facingRight?1:-1;
            p.upgraded=hero.abilities.shadeSoul;
            p.damage=p.upgraded?(hero.charms.shamanStone?40:30):(hero.charms.shamanStone?20:tuning::spiritDamage);
            if(p.upgraded){
                p.speed=tuning::shadeSoulSpeed;p.hitTime=tuning::shadeSoulHitTime;
                const float sx=1.8f*(hero.charms.shamanStone?1.3f:1.f);
                const float sy=1.8f*(hero.charms.shamanStone?1.6f:1.f);
                p.spriteScale={sx,sy};
                p.hitbox={(1.1761009f-2.4178789f*.5f)*sx,(.054492954f-1.5640265f*.5f)*sy,
                          (1.1761009f+2.4178789f*.5f)*sx,(.054492954f+1.5640265f*.5f)*sy};
            }
            p.position={hero.position.x+p.direction*1.2f,hero.position.y};
            p.presentation.reset(p.position);p.life=p.upgraded?tuning::shadeSoulLife:tuning::spiritLife;return;
        }
    }
    void fixedUpdate(float dt,Hero& hero,CombatScene* combat,CombatScene* encounter=nullptr,CombatScene* room=nullptr,
                     void (*geometry)(void*,Rect)=nullptr,void* geometryContext=nullptr){
        for(auto& p:actors)if(p.active()){
            PresentationPosition::Step step(p.presentation,p.position);
            const auto before=p.position;
            const float hitStep=std::min(dt,std::max(0.f,p.hitTime-p.age));
            p.position.x+=p.direction*p.speed*dt;p.age+=dt;p.life=std::max(0.f,p.life-dt);
            if(hitStep<=0)continue;
            const float end=before.x+p.direction*p.speed*hitStep;
            const float left=p.direction>0?p.hitbox.left:-p.hitbox.right;
            const float right=p.direction>0?p.hitbox.right:-p.hitbox.left;
            const Rect swept{std::min(before.x,end)+left,p.position.y+p.hitbox.bottom,
                             std::max(before.x,end)+right,p.position.y+p.hitbox.top};
            // Sweeping prevents a fast spirit skipping thin targets at 50 Hz.
            if(geometry)geometry(geometryContext,swept);
            if(combat)combat->spellHit(swept,p.direction>0?Strike::Right:Strike::Left,p.hit,hero,p.damage);
            if(encounter)encounter->spellHit(swept,p.direction>0?Strike::Right:Strike::Left,p.encounterHit,hero,p.damage);
            if(room&&room!=combat&&room!=encounter)room->spellHit(swept,p.direction>0?Strike::Right:Strike::Left,p.roomHit,hero,p.damage);
        }
    }
private:
    std::uint32_t serial_=0;
};
}
