#include "uumuu_attacks.hpp"
#include "uumuu_attack_data.hpp"
#include <cmath>

namespace hk3ds {
namespace data=uumuu_attack_data;
void UumuuAttacks::reset(){
    phase_=UumuuAttackPhase::Inactive;random_={};timer_=0;
    zapCount_=chaseRepeats_=multizapRepeats_=0;events_={};
}
bool UumuuAttacks::start(EnemyRandom random){
    if(phase_!=UumuuAttackPhase::Inactive)return false;
    random_=random;idle();return true;
}
void UumuuAttacks::idle(){
    phase_=UumuuAttackPhase::Idle;timer_=random_.range(data::idleMin,data::idleMax);
    events_.enterMotion=true;events_.motion=UumuuMotionMode::Idle;
}
void UumuuAttacks::choose(){
    // SendRandomEventV2 rejection-samples choices that reached their repeat cap.
    // Sampling only eligible weights has the same conditional distribution and
    // cannot spin forever when an injected RNG always returns the same value.
    const float chase=chaseRepeats_<data::chaseMax?data::chaseWeight:0;
    const float multi=multizapRepeats_<data::multizapMax?data::multizapWeight:0;
    const bool selectChase=multi==0||(chase>0&&random_.range(0,chase+multi)<chase);
    zapCount_=0;
    events_.enterMotion=true;events_.motion=UumuuMotionMode::Dampen;
    if(selectChase){
        ++chaseRepeats_;multizapRepeats_=0;
        phase_=UumuuAttackPhase::Zapping;timer_=data::zapWait;
    }else{
        ++multizapRepeats_;chaseRepeats_=0;
        phase_=UumuuAttackPhase::Multizap;timer_=data::multizapWait;
        events_.activateRandomPattern=true;
    }
}
void UumuuAttacks::checkQuirrel(UumuuQuirrel& quirrel){
    if(quirrel.check()==UumuuQuirrelDecision::Call){
        quirrel.setTimer(random_);phase_=UumuuAttackPhase::Quirrel;
        events_.callQuirrel=true;
        events_.enterMotion=true;events_.motion=UumuuMotionMode::Coast;
    }else idle();
}
bool UumuuAttacks::afterQuirrelRecovery(UumuuQuirrel& quirrel){
    if(phase_!=UumuuAttackPhase::Quirrel)return false;
    quirrel.roam();checkQuirrel(quirrel);return true;
}
void UumuuAttacks::fixedUpdate(float dt,Vec2 hero,UumuuQuirrel& quirrel){
    if(!std::isfinite(dt)||dt<=0||!std::isfinite(hero.x)||!std::isfinite(hero.y)||
       phase_==UumuuAttackPhase::Inactive||phase_==UumuuAttackPhase::Quirrel)return;
    if(phase_==UumuuAttackPhase::Idle)quirrel.idleUpdate(dt);
    timer_-=dt;
    if(timer_>0)return;
    // Wait actions start a new clock on state entry. Do not carry an oversized
    // timestep through several waits or batch several spawns into one frame.
    switch(phase_){
    case UumuuAttackPhase::Idle:
        phase_=UumuuAttackPhase::Antic;timer_=data::anticWait;events_.antic=true;
        events_.enterMotion=true;events_.motion=UumuuMotionMode::Chase;break;
    case UumuuAttackPhase::Antic:choose();break;
    case UumuuAttackPhase::Zapping:
        // The IntCompare transition is synchronous and aborts the later Spawn.
        if(++zapCount_>data::zapCount){
            phase_=UumuuAttackPhase::Recover;timer_=data::recoverWait;events_.recover=true;
            events_.enterMotion=true;events_.motion=UumuuMotionMode::Coast;
        }else{
            events_.spawnZap=true;events_.zapPosition=hero;
            events_.zapAnticipation=zapCount_==1?data::firstAnticipation:data::laterAnticipation;
            if(zapCount_==1){phase_=UumuuAttackPhase::FirstZapPause;timer_=data::firstZapWait;}
            else timer_=data::zapWait;
            events_.enterMotion=true;events_.motion=zapCount_==1?UumuuMotionMode::Coast:UumuuMotionMode::Dampen;
        }
        break;
    case UumuuAttackPhase::FirstZapPause:
        phase_=UumuuAttackPhase::Zapping;timer_=data::zapWait;
        events_.enterMotion=true;events_.motion=UumuuMotionMode::Dampen;break;
    case UumuuAttackPhase::Multizap:
        phase_=UumuuAttackPhase::Recover;timer_=data::recoverWait;events_.recover=true;
        events_.enterMotion=true;events_.motion=UumuuMotionMode::Coast;break;
    case UumuuAttackPhase::Recover:checkQuirrel(quirrel);break;
    default:break;
    }
}
}
