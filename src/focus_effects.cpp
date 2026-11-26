#include "focus_effects.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hk3ds {
static_assert(sizeof(FocusEffects)<=20u*1024u,"Focus effects heap budget");
bool FocusEffects::load(const char* path,const AnimationBank& bank)
{
    ready_=false;reset();spriteCount_=emitterCount_=0;
    FILE* file=std::fopen(path,"rb");if(!file)return false;
    char magic[4];std::uint16_t counts[2]{};
    bool valid=std::fread(magic,1,4,file)==4&&!std::memcmp(magic,"FOC1",4)&&
        std::fread(counts,2,2,file)==2&&counts[0]>0&&counts[0]<=maxSprites&&counts[1]<=maxEmitters;
    for(unsigned index=0;valid&&index<counts[0];++index){
        char name[48];unsigned kind=0;auto& sprite=sprites_[index];
        valid=std::fread(name,1,48,file)==48&&std::fread(&kind,4,1,file)==1&&
            std::fread(sprite.values.data(),4,6,file)==6&&std::memchr(name,0,48)&&kind<maxSprites;
        if(!valid)break;
        sprite.clip=bank.find(name);sprite.kind=kind;
        const bool looping=kind==0||kind==3;
        valid=sprite.clip&&(looping?sprite.values[4]>=0:sprite.values[4]>0)&&sprite.values[5]>=0;
        for(float value:sprite.values)valid=valid&&std::isfinite(value);
        for(unsigned other=0;other<index;++other)valid=valid&&sprites_[other].kind!=kind;
    }
    for(unsigned index=0;valid&&index<counts[1];++index){
        char name[48];auto& emitter=emitters_[index];
        valid=std::fread(name,1,48,file)==48&&std::fread(&emitter.shape,4,1,file)==1&&
            std::fread(emitter.values.data(),4,15,file)==15&&std::memchr(name,0,48);
        if(!valid)break;
        emitter.clip=bank.find(name);const auto& values=emitter.values;
        valid=emitter.clip&&values[0]>=0&&values[0]<=1000&&values[1]>0&&values[2]>=values[1]&&
            values[2]<=10&&values[3]>=0&&values[4]>=0&&values[5]>=0&&values[7]>=values[6]&&
            values[9]>=values[8]&&values[10]>0&&values[11]>=values[10]&&values[14]>=0;
        for(float value:values)valid=valid&&std::isfinite(value);
    }
    valid=valid&&std::fgetc(file)==EOF;std::fclose(file);
    if(valid){spriteCount_=counts[0];emitterCount_=counts[1];ready_=true;}
    return valid;
}
void FocusEffects::reset()
{
    draws_={};for(auto& particle:particles_)particle.active=false;
    for(auto& emitter:emitters_)emitter.remainder=0;
    previous_=FocusPhase::None;flashAge_=.37f;live_=dropped_=0;seed_=0x464f4355u;
}
float FocusEffects::random(float low,float high)
{seed_=seed_*1664525u+1013904223u;return low+(high-low)*(float(seed_>>8)/16777216.f);}
void FocusEffects::play(unsigned kind)
{
    // Gather/end share Lines Anim; Heal Anim is an independent renderer.
    if(kind<2)for(unsigned index=0;index<spriteCount_;++index)
        if(sprites_[index].kind<2)draws_[index].active=false;
    for(unsigned index=0;index<spriteCount_;++index)if(sprites_[index].kind==kind){
        sprites_[index].age=0;draws_[index].active=true;
        // Lines Anim and Heal Anim use Screen; the ring uses Sprites/Default.
        draws_[index].screenBlend=kind<3;
        draws_[index].followHero=kind!=4;
        // Source: Heal Anim/White Flash R use layer 13; Focus Ring uses 15.
        // Lines Anim stays on layer 0 alongside the hero.
        draws_[index].pass=kind>=2?FocusDrawPass::AfterForeground:FocusDrawPass::BeforeForeground;
        draws_[index].animation.play(sprites_[index].clip,0,true);
    }
}
void FocusEffects::spawn(Emitter& emitter,const Hero& hero)
{
    auto slot=std::find_if(particles_.begin(),particles_.end(),[](const auto& particle){return !particle.active;});
    if(slot==particles_.end()){++dropped_;return;}
    const auto& values=emitter.values;*slot={};slot->active=true;slot->clip=emitter.clip;
    slot->position={hero.position.x+values[12],hero.position.y+values[13]};
    if(emitter.shape==4||emitter.shape==5){
        const float angle=random(0,6.283185307f),radius=std::sqrt(random(0,1))*values[3];
        slot->position.x+=std::cos(angle)*radius*values[4];slot->position.y+=std::sin(angle)*radius*values[5];
    }else{
        slot->position.x+=random(-.5f,.5f)*values[4];slot->position.y+=random(-.5f,.5f)*values[5];
    }
    slot->history.reset(slot->position);slot->velocity={random(values[6],values[7]),random(values[8],values[9])};
    slot->life=random(values[1],values[2]);slot->size=random(values[10],values[11]);slot->fade=values[14];++live_;
}
void FocusEffects::update(float seconds,const Hero& hero)
{
    if(!ready_||!std::isfinite(seconds)||seconds<0)return;
    const auto phase=hero.focus;
    if(hero.actor==Actor::Dead||hero.actor==Actor::Damage){reset();return;}
    if(phase!=previous_){
        if(phase==FocusPhase::Draining){play(0);play(3);}
        if(phase==FocusPhase::Healing){play(2);play(4);flashAge_=0;flashOrigin_=hero.position;}
        if(phase==FocusPhase::Cancelling||phase==FocusPhase::Finishing){
            for(unsigned index=0;index<spriteCount_;++index)if(sprites_[index].kind==0||sprites_[index].kind==3)draws_[index].active=false;
            play(1);
            if(phase==FocusPhase::Cancelling)for(unsigned index=0;index<spriteCount_;++index)
                if(sprites_[index].kind==2)draws_[index].active=false;
        }
        if(phase==FocusPhase::None)for(unsigned index=0;index<spriteCount_;++index)
            if(sprites_[index].kind==0||sprites_[index].kind==3)draws_[index].active=false;
    }
    previous_=phase;
    flashAge_=std::min(.37f,flashAge_+seconds);
    for(unsigned index=0;index<spriteCount_;++index){
        auto& draw=draws_[index];if(!draw.active)continue;
        auto& sprite=sprites_[index];const auto& values=sprite.values;
        const bool looping=sprite.kind==0||sprite.kind==3;
        sprite.age+=seconds;draw.animation.advance(seconds);
        if(!looping&&sprite.age>=values[4]){draw.active=false;continue;}
        const auto origin=draw.followHero?hero.position:flashOrigin_;
        draw.position={origin.x+values[0],origin.y+values[1]};draw.scale={values[2],values[3]};
        draw.alpha=!looping&&values[5]>0?std::clamp((values[4]-sprite.age)/values[5],0.f,1.f):1.f;
    }
    if(seconds==0)return;
    if(live_)for(auto& particle:particles_)if(particle.active){
        PresentationPosition::Step step(particle.history,particle.position);
        particle.age+=seconds;if(particle.age>=particle.life){particle.active=false;--live_;continue;}
        particle.position.x+=particle.velocity.x*seconds;particle.position.y+=particle.velocity.y*seconds;
    }
    if(phase==FocusPhase::Draining||phase==FocusPhase::Healing){
        for(unsigned index=0;index<emitterCount_;++index){auto& emitter=emitters_[index];
            emitter.remainder+=emitter.values[0]*seconds;
            while(emitter.remainder>=1){spawn(emitter,hero);emitter.remainder-=1;}
        }
    }else for(auto& emitter:emitters_)emitter.remainder=0;
}
float FocusEffects::opacity(const FocusParticle& particle)
{return particle.fade>0?std::clamp((particle.life-particle.age)/particle.fade,0.f,1.f):1.f;}
float FocusEffects::healFlash() const
{
    // SpriteFlash.flashFocusHeal: white, .85 amount, .01 up/.01 stay/.35 down.
    return .85f*std::min(std::clamp(flashAge_/.01f,0.f,1.f),std::clamp((.37f-flashAge_)/.35f,0.f,1.f));
}
}
