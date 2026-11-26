#include "nail_interactions.hpp"
#include "breakable.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hk3ds {
bool NailInteractions::load(const char* path,const AnimationBank& bank){
    auto* file=std::fopen(path,"rb");if(!file)return false;
    char magic[4];std::array<Spike,2> spikes;std::array<Prompt,4> prompts;
    const auto read=[&](auto& v){return std::fread(&v,sizeof(v),1,file)==1;};
    const auto rect=[](Rect r){return std::isfinite(r.left)&&std::isfinite(r.bottom)&&std::isfinite(r.right)&&std::isfinite(r.top)&&r.left<r.right&&r.bottom<r.top;};
    bool ok=std::fread(magic,1,4,file)==4&&!std::memcmp(magic,"NIF1",4);
    for(auto& s:spikes){
        ok=ok&&read(s.owner)&&read(s.count)&&s.owner&&s.count>0&&s.count<=2&&read(s.scale)&&std::isfinite(s.scale)&&s.scale>0;
        for(auto& r:s.bounds)ok=ok&&read(r)&&rect(r);
    }
    for(auto& p:prompts)ok=ok&&read(p.owner)&&read(p.position)&&read(p.depth)&&p.owner&&std::isfinite(p.position.x)&&std::isfinite(p.position.y)&&std::isfinite(p.depth);
    ok=ok&&std::fgetc(file)==EOF;std::fclose(file);
    if(!ok||!bank.find("Nail Tink")||!bank.find("Inspect Up")||!bank.find("Inspect Down")||!bank.find("Inspect Label"))return false;
    bind(bank,spikes,prompts);return true;
}
void NailInteractions::bind(const AnimationBank& bank,const std::array<Spike,2>& spikes,const std::array<Prompt,4>& prompts){
    bank_=&bank;spikes_=spikes;prompts_=prompts;reset();
}
void NailInteractions::reset(){states_={};draws_={};prompt_={};promptOwner_=0;promptAge_=labelAlpha_=0;promptDown_=false;random_=0x54494e4bu;}
void NailInteractions::update(float dt,const Hero& hero,const NailBank& nails,unsigned promptOwner,DecorEffects* effects){
    if(!bank_||!std::isfinite(dt)||dt<=0)return;
    for(unsigned i=0;i<states_.size();++i){
        auto& s=states_[i];auto& draw=draws_[i];const auto& c=spikes_[i];
        if(draw.visible){draw.player.advance(dt);if(draw.player.complete)draw.visible=false;}
        s.cooldown=std::max(0.f,s.cooldown-dt);
        const auto& shape=nails.shapes[nailVariant(hero.nail)];
        bool overlaps=false;
        if(hero.nail.active&&hero.nail.hitbox)for(unsigned b=0;b<c.count;++b)overlaps=overlaps||nailOverlaps(hero,shape,c.bounds[b]);
        const bool enter=overlaps&&(!s.overlap||s.attack!=hero.nail.serial);
        s.overlap=overlaps;s.attack=hero.nail.serial;
        if(!enter||s.cooldown>0)continue;
        s.cooldown=.25f;++s.serial;
        random_=random_*1664525u+1013904223u;s.pitch=.85f+.3f*float(random_>>8)/16777216.f;
        // TinkEffect uses the first BoxCollider's edge and the nail Transform.
        const Vec2 nail=nailPoint(hero,shape,{});const Rect r=c.bounds[0];
        draw={};draw.visible=true;draw.scale={c.scale,c.scale};draw.depth=.002f;
        switch(hero.nail.direction){
        case Strike::Right:draw.position={r.left,nail.y};draw.rotation=0;break;
        case Strike::Up:draw.position={nail.x,std::max(r.bottom,nail.y)};draw.rotation=90;break;
        case Strike::Left:draw.position={r.right,nail.y};draw.rotation=180;break;
        case Strike::Down:draw.position={nail.x,std::min(r.top,nail.y)};draw.rotation=270;break;
        }
        draw.player.play(bank_->find("Nail Tink"),0,true);
        if(effects)effects->enemyKillShake();
    }
    if(promptOwner!=promptOwner_){
        if(promptOwner){
            for(const auto& p:prompts_)if(p.owner==promptOwner){
                if(!prompt_.visible)labelAlpha_=0;
                prompt_={};prompt_.visible=true;prompt_.position=p.position;prompt_.depth=p.depth;prompt_.scale={1,1};
                prompt_.player.play(bank_->find("Inspect Up"),0,true);promptAge_=0;promptDown_=false;
            }
        }else if(prompt_.visible){prompt_.player.play(bank_->find("Inspect Down"),0,true);promptAge_=0;promptDown_=true;}
        promptOwner_=promptOwner;
    }else if(prompt_.visible){
        promptAge_+=dt;prompt_.player.advance(dt);
        // Arrow Prompt Labels/FadeGroup: .4s up, .233s down, including Shadow.
        labelAlpha_=std::clamp(labelAlpha_+(promptDown_?-dt/.233f:dt/.4f),0.f,1.f);
        if(promptDown_&&prompt_.player.complete)prompt_.visible=false;
    }
}
float NailInteractions::labelAlpha() const {
    if(!prompt_.visible)return 0;
    return labelAlpha_;
}
void NailInteractions::dispatchAudio(AudioVoices& pool,std::array<unsigned,2>& observed,Vec2 listener) const {
    for(unsigned i=0;i<states_.size();++i)if(observed[i]!=states_[i].serial){
        observed[i]=states_[i].serial;if(!observed[i])continue;
        if(pool.playAtPitch(EnemyClip::NailTink,80+i,states_[i].pitch))for(auto& voice:pool.voices())if(voice.active&&voice.owner==80+i){
            // Approximation: linearize the authored custom rolloff (39..47.06
            // units); retain its exact full-volume and silent distance limits.
            const float d=std::hypot(draws_[i].position.x-listener.x,draws_[i].position.y-listener.y);
            voice.gain=std::clamp((47.058825f-d)/8.058825f,0.f,1.f);
        }
    }
}
}
