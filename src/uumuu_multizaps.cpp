#include "uumuu_multizaps.hpp"
#include "uumuu_multizap_data.hpp"
#include <algorithm>
#include <cmath>

namespace hk3ds {
namespace multizap=uumuu_multizap_data;
static_assert(multizap::count==UumuuMultizaps::capacity,"Source pattern capacity changed");
static_assert(sizeof(UumuuMultizaps)<4096,"Multizap control-state budget exceeded");
Vec2 UumuuMultizaps::position(unsigned i){return i<capacity?multizap::positions[i]:Vec2{};}
bool UumuuMultizaps::activatePattern(unsigned pattern,EnemyRandom random){
    if(pattern>=patternCount)return false;
    for(unsigned i=multizap::offsets[pattern];i<multizap::offsets[pattern+1];++i){
        auto& actor=actors[i];
        // ActivateAllChildren(true) does not re-enable an already active child.
        if(actor.phase!=UumuuMultizapPhase::Inactive)continue;
        actor.scaleX=random.range(0,2)<1?1.f:-1.f;
        actor.rotation=random.range(0,360);
        actor.timer=random.range(0,multizap::delayMax);
        actor.phase=UumuuMultizapPhase::Delay;actor.events={};actor.events.hide=true;
    }
    return true;
}
unsigned UumuuMultizaps::activateRandomPattern(EnemyRandom random){
    const auto pattern=std::min(patternCount-1,unsigned(random.range(0,float(patternCount))));
    activatePattern(pattern,random);return pattern;
}
bool UumuuMultizaps::onAttackEvents(const UumuuAttackEvents& events,EnemyRandom random){
    if(!events.activateRandomPattern)return false;
    activateRandomPattern(random);return true;
}
void UumuuMultizaps::fixedUpdate(float dt){
    if(!std::isfinite(dt)||dt<=0)return;
    for(auto& actor:actors){
        if(actor.phase==UumuuMultizapPhase::Inactive||actor.phase==UumuuMultizapPhase::Ending)continue;
        actor.timer-=dt;if(actor.timer>0)continue;
        switch(actor.phase){
        case UumuuMultizapPhase::Delay:
            actor.phase=UumuuMultizapPhase::Antic;actor.timer=multizap::anticWait;actor.events.antic=true;break;
        case UumuuMultizapPhase::Antic:
            actor.phase=UumuuMultizapPhase::Active;actor.timer=multizap::activeWait;actor.events.strike=true;break;
        case UumuuMultizapPhase::Active:
            actor.phase=UumuuMultizapPhase::Ending;actor.events.end=true;break;
        case UumuuMultizapPhase::EndPause:
            actor.phase=UumuuMultizapPhase::Inactive;actor.events.deactivate=true;break;
        default:break;
        }
    }
}
bool UumuuMultizaps::endAnimationFinished(unsigned i){
    if(i>=capacity||actors[i].phase!=UumuuMultizapPhase::Ending)return false;
    auto& actor=actors[i];actor.phase=UumuuMultizapPhase::EndPause;
    actor.timer=multizap::endPause;actor.events.hide=true;return true;
}
void UumuuMultizaps::deactivate(){
    for(auto& actor:actors){
        if(actor.phase==UumuuMultizapPhase::Inactive)continue;
        actor.phase=UumuuMultizapPhase::Inactive;actor.timer=0;actor.events={};
        actor.events.hide=true;actor.events.deactivate=true;
    }
}
void UumuuMultizaps::contactHero(Hero& hero,Rect body)const{
    if(!std::isfinite(body.left)||!std::isfinite(body.right)||
       !std::isfinite(body.bottom)||!std::isfinite(body.top)||
       body.left>body.right||body.bottom>body.top)return;
    for(unsigned i=0;i<capacity;++i){
        if(actors[i].phase!=UumuuMultizapPhase::Active)continue;
        const auto point=position(i);
        const float dx=point.x-std::clamp(point.x,body.left,body.right);
        const float dy=point.y-std::clamp(point.y,body.bottom,body.top);
        if(dx*dx+dy*dy<=multizap::radius*multizap::radius)
            hero.takeDamage(multizap::damage,point.x<hero.position.x?-1:1);
    }
}
}
