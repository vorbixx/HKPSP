#pragma once
#include "physics.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
namespace hk3ds
{
struct LightingSprite {unsigned width=0,height=0;float left=0,bottom=0,right=0,top=0;};
struct DarknessTrigger {
    Rect bounds{};int region=0;
    std::array<Vec2,8> vertices{};unsigned vertexCount=0;
    bool validPolygon() const {
        if(!vertexCount)return true;
        if(vertexCount<3||vertexCount>vertices.size())return false;
        for(unsigned i=0;i<vertexCount;++i){
            const auto a=vertices[i],b=vertices[(i+1)%vertexCount];
            if(!std::isfinite(a.x)||!std::isfinite(a.y)||a.x<bounds.left||a.x>bounds.right||
               a.y<bounds.bottom||a.y>bounds.top||(a.x==b.x&&a.y==b.y))return false;
            bool interior=false;
            for(unsigned j=0;j<vertexCount;++j){
                const auto p=vertices[j];const float cross=(b.x-a.x)*(p.y-a.y)-(b.y-a.y)*(p.x-a.x);
                if(!std::isfinite(cross)||cross<0)return false;
                interior|=cross>0;
            }
            if(!interior)return false;
        }
        return true;
    }
    bool overlaps(Rect hero) const {
        if(hero.left>=bounds.right||hero.right<=bounds.left||hero.bottom>=bounds.top||hero.top<=bounds.bottom)return false;
        // Separating axes of the authored convex polygon and the hero's box.
        for(unsigned i=0;i<vertexCount;++i){
            const auto a=vertices[i],b=vertices[(i+1)%vertexCount];const float dx=b.x-a.x,dy=b.y-a.y;
            const float x=dy>=0?hero.left:hero.right,y=dx>=0?hero.top:hero.bottom;
            if(dx*(y-a.y)-dy*(x-a.x)<=0)return false;
        }
        return true;
    }
};
struct LightingConfig
{
    std::array<LightingSprite,3> sprites{};
    int sceneDarkness=0; // Runtime SceneManager override, not part of HKL4.
    // Hero scale, offset Y, depth, alpha; vignette offset Y, depth.
    std::array<float,6> transform{};
    // Normal, darkness -1, darkness 1: idle and damage scales.
    std::array<float,8> scales{0,0,0,0,0,0,.8f,.8f};
    std::array<float,3> seconds{};
    static constexpr unsigned triggerCapacity=16;
    std::array<DarknessTrigger,triggerCapacity> triggers{};
    std::array<Rect,4> borders{};
    // Donut scale XY, facing-relative offset X, offset Y, depth and alpha.
    std::array<float,6> glowTransform{};
    // HeroLight Control: damage scale, shrink seconds, hold, recover, death.
    std::array<float,5> heroDamage{};
    std::array<int,4> regionDarkness{0,-1,1,2};
    template<class Regions=std::array<DarknessTrigger,triggerCapacity>>
    void selectSceneDarkness(int level,const Regions& regions={})
    {
        if(level<-1||level>2)return;
        const auto source=scales;sceneDarkness=level;
        const auto scale=[&](int darkness,unsigned damage){
            const unsigned region=darkness==0?0:darkness<0?1:2;
            return darkness==2?.8f:source[region*2+damage];
        };
        // Vignette Control, fsm-20931: Dark 2 and Dark 2 2 both set
        // Normal Scale and Damage Scale to .8 (without the lantern).
        scales[0]=scale(level,0);scales[1]=scale(level,1);triggers={};
        std::array<int,4> levels{level,99,99,99};unsigned count=1,index=0;
        for(const auto& trigger:regions){
            if(trigger.region<-1||trigger.region>2||!std::isfinite(trigger.bounds.left)||!std::isfinite(trigger.bounds.right)||
               !std::isfinite(trigger.bounds.bottom)||!std::isfinite(trigger.bounds.top)||
               trigger.bounds.left>=trigger.bounds.right||trigger.bounds.bottom>=trigger.bounds.top||!trigger.validPolygon())continue;
            if(index==triggers.size())break;
            unsigned slot=0;while(slot<count&&levels[slot]!=trigger.region)++slot;
            if(slot==count&&count==levels.size())continue;
            if(slot==count){levels[count++]=trigger.region;scales[slot*2]=scale(trigger.region,0);scales[slot*2+1]=scale(trigger.region,1);}
            triggers[index]=trigger;triggers[index++].region=int(slot);
        }
        regionDarkness=levels;
    }
    bool load(const char* path)
    {
        *this={};FILE* f=std::fopen(path,"rb");if(!f)return false;
        LightingConfig next;char magic[4];
        bool ok=std::fread(magic,1,4,f)==4&&std::memcmp(magic,"HKL4",4)==0;
        auto floats=[&](float* values,unsigned n){
            if(std::fread(values,4,n,f)!=n)return false;
            for(unsigned i=0;i<n;++i)if(!std::isfinite(values[i])||std::fabs(values[i])>10000)return false;
            return true;
        };
        for(auto& s:next.sprites){
            ok=ok&&std::fread(&s.width,4,1,f)==1&&std::fread(&s.height,4,1,f)==1;
            float b[4];ok=ok&&floats(b,4);
            if(ok){s.left=b[0];s.bottom=b[1];s.right=b[2];s.top=b[3];}
            ok=ok&&s.width>0&&s.height>0&&s.width<=512&&s.height<=512&&s.right>s.left&&s.top>s.bottom;
        }
        ok=ok&&floats(next.transform.data(),6)&&floats(next.scales.data(),6)&&floats(next.seconds.data(),3);
        // HKL4 always contains six original Tutorial triggers. Extra scene
        // partition slots belong to HLS packages, never to this legacy layout.
        for(unsigned i=0;i<6;++i){auto& t=next.triggers[i];float b[4];ok=ok&&floats(b,4)&&std::fread(&t.region,4,1,f)==1;
            if(ok)t.bounds={b[0],b[1],b[2],b[3]};
            ok=ok&&(t.region==1||t.region==2)&&t.bounds.left<t.bounds.right&&t.bounds.bottom<t.bounds.top;}
        for(auto& r:next.borders){float b[4];ok=ok&&floats(b,4);if(ok)r={b[0],b[1],b[2],b[3]};
            ok=ok&&r.left<r.right&&r.bottom<r.top;}
        ok=ok&&floats(next.glowTransform.data(),6)&&next.glowTransform[0]>0&&next.glowTransform[1]>0&&
           next.glowTransform[5]>=0&&next.glowTransform[5]<=1;
        for(float v:next.scales)ok=ok&&v>0;
        for(float v:next.seconds)ok=ok&&v>0;
        ok=ok&&floats(next.heroDamage.data(),5);
        for(float v:next.heroDamage)ok=ok&&v>0;
        ok=ok&&next.transform[0]>0&&next.transform[3]>=0&&next.transform[3]<=1&&std::fgetc(f)==EOF;
        std::fclose(f);if(ok){*this=next;}return ok;
    }
};
// Original Vignette Control FSM20931: positive darkness checks hasLantern.
inline std::array<float,2> sceneDarknessScales(const LightingConfig& source,int darkness,bool lantern)
{
    if(darkness>0&&lantern)return {3.f,2.2f};
    if(darkness>=2)return {.8f,.8f};
    const unsigned region=darkness==0?0:darkness<0?1:2;
    return {source.scales[region*2],source.scales[region*2+1]};
}
class HeroLightState
{
    friend struct SimulationStateHash;
public:
    void reset(const LightingConfig* config){config_=config;scale_=start_=config?config->transform[0]:1;elapsed_=0;age_=-1;phase_=Idle;}
    float scale() const {return scale_;}
    float scaledAge() const {return age_;}
    void step(float seconds,bool damage=false,bool death=false)
    {
        if(!config_||!std::isfinite(seconds)||seconds<0)return;
        if(death&&phase_!=Death){begin(Death);age_=0;}
        else if(damage&&phase_!=Death){begin(Shrink);age_=0;}
        else if(age_>=0)age_+=seconds;
        if(phase_==Idle)return;
        if(phase_==Hold){
            elapsed_+=seconds;
            if(elapsed_>=config_->heroDamage[2])begin(Recover);
            return;
        }
        const float duration=config_->heroDamage[phase_==Shrink?1:phase_==Recover?3:4];
        const float target=phase_==Recover?config_->transform[0]:config_->heroDamage[0];
        // iTween.TweenUpdate applies the previous percentage, then adds the
        // current scaled delta. Completion discards overshoot before FSM Wait.
        scale_=start_+(target-start_)*std::min(elapsed_/duration,1.f);
        if(elapsed_>=duration){
            if(phase_==Shrink)begin(Hold);
            else if(phase_==Recover)phase_=Idle;
        }else elapsed_+=seconds;
    }
private:
    enum Phase {Idle,Shrink,Hold,Recover,Death};
    void begin(Phase phase){phase_=phase;start_=scale_;elapsed_=0;}
    const LightingConfig* config_=nullptr;
    float scale_=1,start_=1,elapsed_=0,age_=-1;
    Phase phase_=Idle;
};
// Vignette FSM startup: a Lantern reveals darkness only when arriving from darkness 0.
inline float sceneLanternRevealStart(int darkness,bool lantern,int previousDarkness){
    return lantern&&previousDarkness==0?(darkness==1?2.2f:darkness==2?1.1f:0.f):0.f;
}
class DarknessState
{
    friend struct SimulationStateHash;
public:
    void reset(const LightingConfig* config,bool lantern=false){
        config_=config;lantern_=lantern;inside_={};region_=0;
        scale_=start_=target_=config?config->scales[0]:1;
        elapsed_=duration_=pause_=0;reveal_=false;revealAge_=0;
        glowActive_=lantern&&config&&config->sceneDarkness>0;glowShrinking_=false;
        glowScale_=glowActive_?1.f:.3f;glowAge_=glowShrinkAge_=0;
    }
    void beginLanternReveal(float start,bool sceneEntry=false){
        if(!config_||start<=0)return;
        scale_=revealStart_=start;revealAge_=0;reveal_=true;
        if(sceneEntry)glowScale_=1.f;
        if(!glowActive_)glowAge_=0;
        glowActive_=true;glowShrinking_=false;
        glowStart_=glowScale_;
    }
    float scale() const {return scale_;}
    int region() const {return region_;}
    bool lanternVisible() const {return config_&&lantern_&&config_->regionDarkness[region_]>0;}
    float lanternGlowScale() const {return glowActive_?glowScale_:0.f;}
    float lanternGlowAge() const {return glowAge_;}
    void step(Rect hero,float seconds,bool damage=false){step(hero,seconds,damage,lantern_);}
    void step(Rect hero,float seconds,bool damage,bool lantern)
    {
        if(!config_)return;
        const bool wasVisible=lanternVisible();
        if(lantern_!=lantern){
            lantern_=lantern;reveal_=false;pause_=0;
            if(lanternVisible())beginLanternReveal(scale_);
            else transition(idleScale(),config_->seconds[2]);
        }
        std::array<bool,4> inside{};
        for(const auto& t:config_->triggers)
            if(t.overlaps(hero))inside[t.region]=true;
        // Entries select the new region; an old region's exit must not erase a
        // simultaneous entry. Keep another overlapping region active on exit.
        int next=region_;
        for(int i=1;i<=3;++i)if(inside[i]&&!inside_[i])next=i;
        if(!inside[next])next=inside[3]?3:inside[2]?2:inside[1]?1:0;
        if(next!=region_){
            // SCENE RESET preserves the current radius for Lantern reveals.
            region_=next;reveal_=false;pause_=0;
            if(lanternVisible())beginLanternReveal(scale_);
            else transition(idleScale(),config_->seconds[2]);
        }
        inside_=inside;
        const float glowSeconds=std::max(0.f,seconds);
        if(lanternVisible()){
            if(!wasVisible){if(!glowActive_)glowAge_=0;glowActive_=true;glowShrinking_=false;}
        }else if(wasVisible&&glowActive_){
            // Prefab7964 Shrink FSM: Linear to .5 in 1.25s, then deactivate.
            glowShrinking_=true;glowShrinkAge_=0;glowShrinkStart_=glowScale_;
        }
        if(glowActive_)glowAge_+=glowSeconds;
        if(glowShrinking_){
            glowShrinkAge_+=glowSeconds;
            const float t=std::clamp(glowShrinkAge_/1.25f,0.f,1.f);
            glowScale_=glowShrinkStart_+(.5f-glowShrinkStart_)*t;
            if(t>=1)glowActive_=glowShrinking_=false;
        }
        if(reveal_&&!damage){
            revealAge_+=std::max(0.f,seconds);
            const float t=std::clamp((revealAge_-.5f)/4.f,0.f,1.f);
            const float eased=std::sin(t*1.57079632679f);
            scale_=revealStart_+(idleScale()-revealStart_)*eased;
            glowScale_=glowStart_+(3.f-glowStart_)*eased;
            if(t>=1){reveal_=false;start_=target_=scale_;elapsed_=duration_=pause_=0;}
            return;
        }
        reveal_=false;
        if(damage){transition(lanternVisible()?2.2f:config_->scales[region_*2+1],config_->seconds[0]);pause_=config_->seconds[1];}
        float remaining=std::max(0.f,seconds);
        while(remaining>0){
            if(elapsed_<duration_){const float dt=std::min(remaining,duration_-elapsed_);elapsed_+=dt;remaining-=dt;
                scale_=start_+(target_-start_)*(elapsed_/duration_);}
            else if(pause_>0){const float dt=std::min(remaining,pause_);pause_-=dt;remaining-=dt;
                if(pause_==0)transition(idleScale(),config_->seconds[2]);}
            else break;
        }
    }
private:
    float idleScale() const {return lanternVisible()?3.f:config_->scales[region_*2];}
    void transition(float target,float duration){start_=scale_;target_=target;duration_=duration;elapsed_=0;}
    const LightingConfig* config_=nullptr;
    std::array<bool,4> inside_{};
    int region_=0;
    float scale_=1,start_=1,target_=1,elapsed_=0,duration_=0,pause_=0;
    float revealAge_=0,revealStart_=1;bool reveal_=false,lantern_=false;
    float glowScale_=.3f,glowStart_=.3f,glowAge_=0;
    float glowShrinkAge_=0,glowShrinkStart_=.3f;bool glowActive_=false,glowShrinking_=false;
};
}
