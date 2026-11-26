#include "hud.hpp"
#include "hud_liquid_data.hpp"
#include "hud_limiter_data.hpp"
#include "decor_motion.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>

namespace hk3ds {
namespace {
HudSnapshot bounded(HudSnapshot value)
{
    value.maxHealth=std::clamp(value.maxHealth,0,static_cast<int>(HudPresentation::maskCapacity));
    value.health=std::clamp(value.health,0,value.maxHealth);
    value.blueHealth=std::clamp(value.blueHealth,0,static_cast<int>(HudPresentation::maskCapacity)-value.maxHealth);
    value.maxSoul=std::max(0,value.maxSoul);
    value.soul=std::clamp(value.soul,0,value.maxSoul);
    return value;
}
HudMaskKind maskKind(const HudSnapshot& value,unsigned index)
{
    if(index<static_cast<unsigned>(value.maxHealth))
        return index<static_cast<unsigned>(value.health)?HudMaskKind::White:HudMaskKind::Empty;
    return index<static_cast<unsigned>(value.maxHealth+value.blueHealth)?HudMaskKind::Blue:HudMaskKind::Hidden;
}
bool full(HudMaskKind kind) { return kind==HudMaskKind::White||kind==HudMaskKind::Blue; }
float limiterCurve(const float* curve,float age,float life)
{
    const float sample=std::clamp(age/life,0.f,1.f)*31;
    const unsigned i=std::min(30u,static_cast<unsigned>(sample));
    return curve[i]+(curve[i+1]-curve[i])*(sample-i);
}
bool equal(const HudSnapshot& a,const HudSnapshot& b)
{
    return a.health==b.health&&a.maxHealth==b.maxHealth&&a.blueHealth==b.blueHealth&&
        a.soul==b.soul&&a.maxSoul==b.maxSoul&&a.geo==b.geo&&a.soulDrain==b.soulDrain&&a.soulRecovery==b.soulRecovery;
}
}
float HudPresentation::LimiterFragment::currentSize() const
{ return visible()?size*limiterCurve(huddata::limiterSize,age,life):0; }
float HudPresentation::LimiterFragment::alpha() const
{ return visible()?limiterCurve(huddata::limiterAlpha,age,life):0; }
float HudRefillParticle::positionY() const
{
    const auto& e=huddata::refillEmitters[emitter];
    const float t=std::max(0.f,age);
    // Approximation: integrate the source .1 velocity damping at 60 Hz as a
    // continuous decay. Birth count, line shape, ranges and lifetime are source.
    const float decay=-60.f*std::log(1.f-e.dampen);
    const float travel=e.dampen<=0||speed<=limit?speed*t:limit*t+(speed-limit)*(1-std::exp(-decay*t))/decay;
    return y+e.direction*travel+.5f*force*t*t;
}
float HudRefillParticle::currentSize() const
{
    if(!visible())return 0;
    const float index=std::clamp(age/life,0.f,1.f)*31;
    const unsigned i=std::min(30u,static_cast<unsigned>(index));
    const auto sample=[i,index](const float* curve){return curve[i]+(curve[i+1]-curve[i])*(index-i);};
    const float low=sample(huddata::refillSizeMin),high=sample(huddata::refillSizeMax);
    return size*(low+(high-low)*curveMix);
}
float HudPresentation::feedbackRandom()
{
    // Approximation: deterministic local RNG instead of Unity's global stream.
    feedbackSeed_=feedbackSeed_*1664525u+1013904223u;
    return (feedbackSeed_>>8)*(1.f/16777216.f);
}
void HudPresentation::emitFeedback(unsigned mask,unsigned first,unsigned end)
{
    for(unsigned emitter=first;emitter<end;++emitter){
        const auto& e=huddata::refillEmitters[emitter];
        const unsigned count=static_cast<unsigned>(std::lround(e.rate*e.seconds));
        for(unsigned i=0;i<count;++i){
            auto slot=std::find_if(refill_.begin(),refill_.end(),[](const auto& p){return p.life<=0;});
            if(slot==refill_.end()){++refillDrops_;continue;}
            const auto range=[this](float a,float b){return a+(b-a)*feedbackRandom();};
            float x=e.x,y=e.y;
            if(emitter==2){const float angle=feedbackRandom()*6.28318530718f;x+=std::cos(angle)*e.radius;y+=std::sin(angle)*e.radius;}
            else x+=range(-e.radius,e.radius);
            *slot={-e.delay-static_cast<float>(i+1)/e.rate,range(e.lifeMin,e.lifeMax),x,y,
                   range(e.speedMin,e.speedMax),range(e.limitMin,e.limitMax),range(e.sizeMin,e.sizeMax),feedbackRandom(),
                   range(e.forceMin,e.forceMax),mask,emitter};
        }
    }
}
HudSnapshot HudSnapshot::from(const Vitals& vitals,std::uint32_t geo,bool soulDrain,bool soulRecovery)
{ return {vitals.health,vitals.maxHealth,vitals.blueHealth,vitals.soul,vitals.maxSoul,geo,soulDrain,soulRecovery}; }
void HudPresentation::reset(const HudSnapshot& observed)
{
    snapshot_=bounded(observed);soul_=soulStart_=static_cast<float>(snapshot_.soul);soulClock_=0;geo_=snapshot_.geo;geoClock_=0;geoStep_=1;
    liquidAnimation_=HudLiquidAnimation::Idle;liquidClock_=0;
    soulFlashClock_=huddata::soulFlashSeconds;soulBurstClock_=huddata::soulBurstSeconds;soulFullClock_=0;
    refill_={};refillDrops_=0;feedbackSeed_=1;
    limiter_={};limiterBurstClock_=huddata::limiterBurstSeconds;
    canFocus_=snapshot_.soul>=33;
    liquidTone_=liquidToneStart_=snapshot_.soul>=33?1.f:110.f/255;liquidToneClock_=.2f;
    eyesOver_=snapshot_.soul>=50;eyesClock_=1;
    eyesAlpha_=eyesStartAlpha_=eyesOver_?1.f:0.f;eyesScale_=eyesStartScale_=eyesOver_?1.f:.4f;
    // firstGeo survives Retry/scene refresh; only transient roller state resets.
    geoVisible_=geoVisible_||snapshot_.geo>0;geoRevealClock_=1;
    geoDelayClock_=geoRollClock_=0;geoCounting_=geoPendingUp_=false;
    geoPendingClock_=.25f;geoPendingAlpha_=geoPendingStartAlpha_=0;geoPendingValue_=0;
    for(unsigned i=0;i<maskCapacity;++i)masks_[i]={maskKind(snapshot_,i),maskKind(snapshot_,i),HudMaskAnimation::None,0};
    initialized_=true;
}
bool HudPresentation::update(const HudSnapshot& observed,float seconds)
{
    if(!initialized_) { reset(observed);return true; }
    const float dt=std::isfinite(seconds)?std::max(0.0f,seconds):0.0f;
    bool changed=false;
    if(dt>0){
        if(limiterBurstClock_<huddata::limiterBurstSeconds){limiterBurstClock_=std::min(huddata::limiterBurstSeconds,limiterBurstClock_+dt);changed=true;}
        for(auto& p:limiter_)if(p.life>0){
            const float elapsed=std::max(0.f,std::min(p.age+dt,p.life))-std::max(0.f,p.age);
            float remaining=elapsed;
            // Share the measured native 3D clamp: invisible depth velocity must
            // participate in speed limiting before projecting onto the HUD.
            while(remaining>0){
                const float step=std::min(remaining,1.f/60);remaining=std::max(0.f,remaining-step);
                std::array<float,3> position{p.x,p.y,0},velocity{p.vx,p.vy,p.vz};
                decorMotionStep(position,velocity,{}, {0,p.force*step,0}, {0,huddata::limiterGravity*step,0},
                    step,huddata::limiterLimit,decorRetention(huddata::limiterDampen,step));
                p.x=position[0];p.y=position[1];p.vx=velocity[0];p.vy=velocity[1];p.vz=velocity[2];
            }
            p.age+=dt;if(p.age>=p.life)p.life=0;changed=true;
        }
        if(soulFlashClock_<huddata::soulFlashSeconds){soulFlashClock_=std::min(huddata::soulFlashSeconds,soulFlashClock_+dt);changed=true;}
        if(soulBurstClock_<huddata::soulBurstSeconds){soulBurstClock_=std::min(huddata::soulBurstSeconds,soulBurstClock_+dt);changed=true;}
        if(soulFull()){soulFullClock_=std::fmod(soulFullClock_+dt,2*huddata::soulPulseSeconds);changed=true;}
        for(auto& p:refill_)if(p.life>0){p.age+=dt;if(p.age>=p.life)p.life=0;changed=true;}
    }
    if(geoVisible_&&geoRevealClock_<.45f&&dt>0){geoRevealClock_=std::min(.45f,geoRevealClock_+dt);changed=true;}
    if(geoPendingClock_<.25f&&dt>0){
        geoPendingClock_=std::min(.25f,geoPendingClock_+dt);
        geoPendingAlpha_=geoPendingStartAlpha_+((geoPendingUp_?1.f:0.f)-geoPendingStartAlpha_)*(geoPendingClock_/.25f);changed=true;
    }
    liquidClock_+=dt;
    for(auto& mask:masks_)if(mask.animation!=HudMaskAnimation::None&&dt>0)
    {
        const bool blue=mask.kind==HudMaskKind::Blue||mask.previous==HudMaskKind::Blue;
        const float duration=mask.animation==HudMaskAnimation::Break?
            (blue?timing_.blueBreakSeconds:timing_.maskBreakSeconds):
            (blue?timing_.blueRefillSeconds:timing_.maskRefillSeconds);
        mask.elapsed+=dt;changed=true;
        if(mask.elapsed>=duration) { mask.animation=HudMaskAnimation::None;mask.previous=mask.kind;mask.elapsed=0; }
    }
    const auto next=bounded(observed);
    if(!equal(next,snapshot_))changed=true;
    if(next.soulLimited()!=snapshot_.soulLimited()){
        limiterBurstClock_=0;
        if(next.soulLimited()){
            static_assert(limiterCapacity==huddata::limiterCapacity,"source limiter particle cap");
            const auto range=[this](float a,float b){return a+(b-a)*feedbackRandom();};
            const unsigned count=static_cast<unsigned>(std::lround(huddata::limiterRate*huddata::limiterEmissionSeconds));
            assert(count<=limiterCapacity);
            for(unsigned i=0;i<count;++i){
                const float azimuth=range(0,6.28318530718f),radius=std::sqrt(feedbackRandom())*huddata::limiterRadius;
                const float angle=std::acos(1-feedbackRandom()*(1-std::cos(huddata::limiterAngle)));
                const float speed=range(huddata::limiterSpeedMin,huddata::limiterSpeedMax);
                limiter_[i]={-static_cast<float>(i+1)/huddata::limiterRate,range(huddata::limiterLifeMin,huddata::limiterLifeMax),
                    huddata::limiterX+radius*std::sin(azimuth),huddata::limiterY,
                    speed*std::sin(angle)*std::sin(azimuth),speed*std::cos(angle),speed*std::sin(angle)*std::cos(azimuth),
                    range(huddata::limiterSizeMin,huddata::limiterSizeMax),range(huddata::limiterRotationMin,huddata::limiterRotationMax),
                    range(huddata::limiterForceMin,huddata::limiterForceMax),static_cast<unsigned>(feedbackRandom()*3)};
            }
        }
    }
    for(unsigned i=0;i<maskCapacity;++i)
    {
        auto& mask=masks_[i];const auto kind=maskKind(next,i);
        if(kind==mask.kind)continue;
        mask.previous=mask.kind;mask.kind=kind;mask.elapsed=0;
        mask.animation=full(kind)?HudMaskAnimation::Refill:full(mask.previous)?HudMaskAnimation::Break:HudMaskAnimation::None;
        // health_display Heal? creates HP Up Particles, not Blue Appear/Max Up.
        if(kind==HudMaskKind::White&&mask.previous==HudMaskKind::Empty)emitFeedback(i,0,2);
    }
    if(next.geo!=snapshot_.geo)
    {
        // Geo Sprite's First Geo Uncollected -> First Get enables both renderers.
        // Keep the reveal latched when the balance later reaches zero.
        if(!geoVisible_&&next.geo>snapshot_.geo){geoVisible_=true;geoRevealClock_=0;}
        // A loaded/spent balance never counts backwards as a pickup.
        geoPendingStartAlpha_=geoPendingAlpha_;geoPendingClock_=0;
        if(next.geo<snapshot_.geo) { geo_=next.geo;geoClock_=0;geoCounting_=geoPendingUp_=false;geoPendingValue_=0; }
        else {
            geoDelayClock_=std::max(0.0f,timing_.geoDelaySeconds);geoCounting_=false;geoRollClock_=0;
            geoPendingUp_=true;geoPendingValue_=next.geo-geo_;
            // GeoCounter.AddGeo computes once, including earlier pending pickups.
            geoStep_=std::max<std::uint32_t>(1,static_cast<std::uint32_t>(static_cast<double>(next.geo-geo_)*.025*1.75));
        }
    }
    if(next.soul!=snapshot_.soul)
    {
        if(next.soul>snapshot_.soul){
            // White Flash is a separate FSM: FLASH is ignored while Flash runs.
            if(next.soul<next.maxSoul&&soulFlashClock_>=huddata::soulFlashSeconds)soulFlashClock_=0;
            if(next.maxSoul>=99&&next.soul>=next.maxSoul&&!soulFull()){
                soulFullClock_=0;soulBurstClock_=0;
            }
        }
        soulStart_=soul_;soulClock_=0;
        // Soul Orb MP Gain and MP Drain use SetPosition, MP Lose is tweened.
        if(next.soul>snapshot_.soul||next.soulDrain||snapshot_.soulDrain)soul_=static_cast<float>(next.soul);
        else { liquidAnimation_=HudLiquidAnimation::Shrink;liquidClock_=0; }
    }
    if((next.soul>=33)!=(snapshot_.soul>=33)) { liquidToneStart_=liquidTone_;liquidToneClock_=0; }
    // Soul Orb Check Can Heal cancels during Focus. Can Heal 2 plays its
    // non-looping circle emitter only when Can Focus Anim was previously false.
    if(!next.soulDrain){
        if(next.soul>=33&&!canFocus_)emitFeedback(maskCapacity,2,3);
        canFocus_=next.soul>=33;
    }
    if(liquidToneClock_<.2f&&dt>0) {
        liquidToneClock_=std::min(.2f,liquidToneClock_+dt);
        const float targetTone=next.soul>=33?1.f:110.f/255;
        liquidTone_=liquidToneStart_+(targetTone-liquidToneStart_)*(liquidToneClock_/.2f);changed=true;
    }
    snapshot_=next;
    const float target=static_cast<float>(snapshot_.soul);
    if(soul_!=target&&dt>0)
    {
        soulClock_+=dt;
        const float progress=timing_.soulTweenSeconds>0?std::min(1.0f,soulClock_/timing_.soulTweenSeconds):1.0f;
        // iTween easeType 13 is easeOutSine (Soul Orb MP Lose).
        const float eased=progress>=1?1.0f:std::sin(progress*1.57079632679f);
        soul_=snapshot_.soulDrain?target:soulStart_+(target-soulStart_)*eased;changed=true;
    }
    // MP Lose goes through Check Eyes 2 before starting its liquid tween.
    // Eye visibility follows the logical charge, independently of liquid height.
    if((snapshot_.soul>=50)!=eyesOver_){
        eyesOver_=snapshot_.soul>=50;eyesClock_=0;eyesStartAlpha_=eyesAlpha_;eyesStartScale_=eyesScale_;
    }
    const float eyeFade=eyesOver_?.8f:.2f,eyeScaleTime=eyesOver_?.5f:.2f;
    if(eyesClock_<eyeFade&&dt>0){
        eyesClock_+=dt;
        const auto ease=[](float t){return std::sin(std::min(1.f,t)*1.57079632679f);};
        const float fade=std::min(1.f,eyesClock_/eyeFade);
        eyesAlpha_=eyesStartAlpha_+((eyesOver_?1.f:0.f)-eyesStartAlpha_)*(eyesOver_?fade:ease(fade));
        eyesScale_=eyesStartScale_+((eyesOver_?1.f:.4f)-eyesStartScale_)*ease(eyesClock_/eyeScaleTime);changed=true;
    }
    if(geo_<snapshot_.geo&&dt>0)
    {
        // GeoCounter.Update subtracts dt only on non-tick frames. A tick adds
        // .025 without consuming dt, so this is slower than a 40 Hz accumulator.
        if(!geoCounting_){
            if(geoDelayClock_>0)geoDelayClock_-=dt;
            else geoCounting_=true;
        }
        if(geoCounting_){
            geoRollClock_+=dt;
            if(geoClock_<0){
                geo_+=std::min(snapshot_.geo-geo_,geoStep_);changed=true;
                geoPendingValue_=snapshot_.geo-geo_;
                geoClock_+=std::max(.001f,timing_.geoStepSeconds);
                if(geo_==snapshot_.geo){
                    geoCounting_=geoPendingUp_=false;geoPendingStartAlpha_=geoPendingAlpha_;geoPendingClock_=0;
                }
            }else geoClock_-=dt;
        }
    }
    return changed;
}
float HudPresentation::soulFraction() const
{ return std::clamp(soul_/99.0f,0.0f,1.0f); }
float HudPresentation::soulFlashAlpha() const
{
    const float t=std::min(1.f,soulFlashClock_/huddata::soulFlashSeconds);
    return huddata::soulFlashAlpha*(1-std::sin(t*1.57079632679f));
}
float HudPresentation::soulRimAlpha() const
{
    if(!soulFull())return 0;
    const float t=soulFullClock_/huddata::soulPulseSeconds;
    return t<=1?t:2-t;
}
float HudPresentation::geoTextAlpha() const
{ return geoVisible_?std::min(1.f,geoRevealClock_/.25f):0.f; }
bool HudPresentation::animating() const
{
    if(limiterBurstClock_<huddata::limiterBurstSeconds)return true;
    for(const auto& p:limiter_)if(p.life>0)return true;
    if(soulFull()||soulFlashClock_<huddata::soulFlashSeconds||soulBurstClock_<huddata::soulBurstSeconds)return true;
    for(const auto& p:refill_)if(p.life>0)return true;
    if(geoVisible_&&geoRevealClock_<.45f)return true;
    if(geoPendingClock_<.25f)return true;
    if(liquidToneClock_<.2f||eyesClock_<(eyesOver_?.8f:.2f))return true;
    if(soul_!=static_cast<float>(snapshot_.soul)||geo_!=snapshot_.geo)return true;
    for(const auto& mask:masks_)if(mask.animation!=HudMaskAnimation::None)return true;
    return false;
}
}
