#include "uumuu_zap.hpp"
#include "uumuu_zap_data.hpp"
#include <algorithm>
#include <cmath>

namespace hk3ds {
namespace data=uumuu_zap_data;
bool UumuuZap::activate(Vec2 position){
    if(phase_!=UumuuZapPhase::Inactive||!std::isfinite(position.x)||!std::isfinite(position.y))return false;
    position_=position;phase_=UumuuZapPhase::Antic;timer_=data::anticWait;
    events_={};events_.antic=true;return true;
}
bool UumuuZap::onAttackEvents(const UumuuAttackEvents& events){
    // The exported prefab has no Control FSM: Antic Time is an unconsumed FSM
    // request. MegaJellyZap.ZapSequence supplies its own fixed coroutine waits.
    return events.spawnZap&&activate(events.zapPosition);
}
void UumuuZap::fixedUpdate(float dt){
    if(!std::isfinite(dt)||dt<=0||phase_==UumuuZapPhase::Inactive)return;
    timer_-=dt;if(timer_>0)return;
    // Each coroutine wait begins on entry; preserve a visible/source frame for
    // each transition instead of skipping particle/audio events on large dt.
    switch(phase_){
    case UumuuZapPhase::Antic:
        phase_=UumuuZapPhase::Active;timer_=data::activeWait;events_.strike=true;break;
    case UumuuZapPhase::Active:
        phase_=UumuuZapPhase::EndPause;timer_=data::endPause;events_.end=true;break;
    case UumuuZapPhase::EndPause:
        phase_=UumuuZapPhase::Inactive;events_.recycle=true;break;
    default:break;
    }
}
void UumuuZap::deactivate(){
    // OnDisable cancels the coroutine. The room/pool adapter disables visuals;
    // cancelling does not run the source sequence's later sound or Recycle.
    phase_=UumuuZapPhase::Inactive;timer_=0;events_={};
}
void UumuuZap::contactHero(Hero& hero,Rect body)const{
    if(phase_!=UumuuZapPhase::Active||!std::isfinite(body.left)||!std::isfinite(body.right)||
       !std::isfinite(body.bottom)||!std::isfinite(body.top)||body.left>body.right||body.bottom>body.top)return;
    const float dx=position_.x-std::clamp(position_.x,body.left,body.right);
    const float dy=position_.y-std::clamp(position_.y,body.bottom,body.top);
    if(dx*dx+dy*dy<=data::radius*data::radius)
        hero.takeDamage(data::damage,position_.x<hero.position.x?-1:1);
}
}
