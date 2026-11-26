#include "uumuu_wake.hpp"
#include "uumuu_wake_data.hpp"
#include <algorithm>
#include <cmath>

namespace hk3ds {
static_assert(sizeof(UumuuWake)<=128,"Uumuu wake exceeded its fixed-state estimate");
namespace { bool validWakeDt(float dt){return std::isfinite(dt)&&dt>0;} }
bool UumuuWake::battleStart(){
    if(phase_!=UumuuWakePhase::Sleep)return false;
    phase_=UumuuWakePhase::Pause;timer_=uumuu_wake_data::wakePause;return true;
}
void UumuuWake::controlUpdate(float dt){
    if(!validWakeDt(dt)||(phase_!=UumuuWakePhase::Pause&&
       phase_!=UumuuWakePhase::Rumble&&phase_!=UumuuWakePhase::Start))return;
    timer_-=dt;if(timer_>0)return;
    // A newly entered Wait never consumes the preceding callback's excess dt.
    if(phase_==UumuuWakePhase::Pause){
        phase_=UumuuWakePhase::Rumble;timer_=uumuu_wake_data::rumbleWait;events_.rumble=true;
    }else if(phase_==UumuuWakePhase::Rumble){
        phase_=UumuuWakePhase::Burst;events_.burst=true;
    }else{phase_=UumuuWakePhase::Ready;events_.ready=true;}
}
bool UumuuWake::tweenUpdate(float dt,Vec2& transform){
    if(phase_!=UumuuWakePhase::Burst||!validWakeDt(dt)||
       !std::isfinite(transform.x)||!std::isfinite(transform.y))return false;
    const float elapsed=std::min(uumuu_wake_data::burstDuration,tweenElapsed_+dt);
    const float t=elapsed/uumuu_wake_data::burstDuration-1;
    const float offset=uumuu_wake_data::burstDeltaY*(t*t*t+1); // easeOutCubic (4).
    const float nextY=transform.y+(offset-tweenOffset_);
    if(!std::isfinite(nextY))return false;
    transform.y=nextY;tweenElapsed_=elapsed;tweenOffset_=offset;
    if(elapsed>=uumuu_wake_data::burstDuration){
        events_.music=true;events_.start=true;
        phase_=UumuuWakePhase::Start;timer_=uumuu_wake_data::startWait;
    }
    return true;
}
}
