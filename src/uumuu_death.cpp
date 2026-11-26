#include "uumuu_death.hpp"
#include "uumuu_death_data.hpp"
#include <cmath>

namespace hk3ds {
namespace data=uumuu_death_data;
bool UumuuDeath::activate(bool bossScene){
    if(phase_!=UumuuDeathPhase::Inactive)return false;
    events_={};events_.defeatedMegaJelly=true;
    // GGCheckIfBossScene skips the music action in boss scenes. Both actions
    // finish immediately; snapshot transition duration is not a coroutine wait.
    events_.musicSnapshot=!bossScene;
    phase_=UumuuDeathPhase::Init;timer_=data::initWait;events_.init=true;return true;
}
void UumuuDeath::fixedUpdate(float dt){
    if(!std::isfinite(dt)||dt<=0||phase_==UumuuDeathPhase::Inactive||phase_==UumuuDeathPhase::Blown)return;
    timer_-=dt;if(timer_>0)return;
    switch(phase_){
    case UumuuDeathPhase::Init:
        phase_=UumuuDeathPhase::Steam;timer_=data::steamWait;events_.steam=true;break;
    case UumuuDeathPhase::Steam:
        phase_=UumuuDeathPhase::Ready;timer_=data::readyWait;events_.ready=true;break;
    case UumuuDeathPhase::Ready:
        phase_=UumuuDeathPhase::Blown;events_.blow=true;events_.bossDeath=true;break;
    default:break;
    }
}
}
