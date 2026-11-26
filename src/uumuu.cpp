#include "uumuu.hpp"
#include "uumuu_data.hpp"
#include "uumuu_death.hpp"
#include <cmath>

namespace hk3ds {
bool UumuuShell::bind(CombatObject& object){
    // Do not replace another domain's hit receiver.
    if((object.receiver||object.onHit)&&(object.receiver!=this||object.onHit!=receive))return false;
    reset();previousHealth_=object.health;previousHasHealth_=object.hasHealth;
    previousBounceShroom_=object.bounceShroom;
    object_=&object;object.hasHealth=true;
    object.health={};object.health.hp=uumuu_data::hp;object.health.enemyType=uumuu_data::enemyType;
    object.health.invincible=uumuu_data::initialInvincible;
    object.health.invincibleFromDirection=uumuu_data::initialDirection;
    object.bounceShroom=uumuu_data::initialBounceShroom;
    object.receiver=this;object.onHit=receive;
    return true;
}
void UumuuShell::reset(){
    if(object_&&object_->receiver==this&&object_->onHit==receive){
        object_->receiver=nullptr;object_->onHit=nullptr;
        object_->health=previousHealth_;object_->hasHealth=previousHasHealth_;
        object_->bounceShroom=previousBounceShroom_;
    }
    object_=nullptr;phase_=UumuuShellPhase::Guarded;timer_=0;lethal_=false;
    pendingDestroy_=0;destroyRequest_=false;
}
bool UumuuShell::beginSlash(){
    if(!object_||phase_!=UumuuShellPhase::Guarded||object_->health.dead)return false;
    phase_=UumuuShellPhase::Slash;object_->health.invincible=false;
    object_->health.invincibleFromDirection=uumuu_data::slashDirection;
    // Slash requests Object.Destroy. Presence remains until native destruction.
    if(++destroySerial_==0)++destroySerial_;
    pendingDestroy_=destroySerial_;destroyRequest_=true;
    return true;
}
bool UumuuShell::bounceShroomDestroyed(std::uint32_t token){
    if(!token||token!=pendingDestroy_||!object_||
       object_->receiver!=this||object_->onHit!=receive)return false;
    object_->bounceShroom=uumuu_data::slashBounceShroom;pendingDestroy_=0;
    destroyRequest_=false;return true;
}
bool UumuuShell::slashAnimationFinished(){
    if(!object_||phase_!=UumuuShellPhase::Slash)return false;
    phase_=UumuuShellPhase::Wounded;timer_=uumuu_data::woundedWait;return true;
}
void UumuuShell::fixedUpdate(float dt){
    if(!object_||!std::isfinite(dt)||dt<=0)return;
    if(phase_!=UumuuShellPhase::Wounded&&phase_!=UumuuShellPhase::RecoverAntic&&phase_!=UumuuShellPhase::Recover)return;
    timer_-=dt;
    if(timer_<=0){
        // Each authored Wait starts a new clock on entry, as in the attack FSM.
        switch(phase_){
        case UumuuShellPhase::Wounded:
            phase_=UumuuShellPhase::RecoverAntic;timer_=uumuu_data::recoverAnticWait;break;
        case UumuuShellPhase::RecoverAntic:
            phase_=UumuuShellPhase::Recover;timer_=uumuu_data::recoverWait;
            object_->health.invincible=true;object_->health.invincibleFromDirection=uumuu_data::recoverDirection;
            // AddComponent creates a new component synchronously. An old
            // destruction callback cannot remove this replacement.
            object_->bounceShroom=uumuu_data::recoverBounceShroom;
            pendingDestroy_=0;destroyRequest_=false;break;
        case UumuuShellPhase::Recover:phase_=UumuuShellPhase::Guarded;return;
        default:return;
        }
    }
}
void UumuuShell::receive(void* context,const NailHit&,HitOutcome outcome){
    auto& self=*static_cast<UumuuShell*>(context);
    if(outcome==HitOutcome::Killed&&self.phase_!=UumuuShellPhase::Dead){
        self.phase_=UumuuShellPhase::Dead;self.lethal_=true;
    }
}
void UumuuBattle::bind(bool defeated){
    stage_=defeated?Stage::Complete:Stage::Idle;timer_=0;encountered_=false;
    events_={};
    if(defeated){events_.quickOpenGates=true;events_.deactivateChildren=true;}
}
bool UumuuBattle::arenaEntered(){
    if(stage_!=Stage::Idle)return false;
    stage_=Stage::Active;encountered_=true;
    events_.encounteredMegaJelly=true;events_.battleStart=true;
    events_.closeGates=true;events_.activateChildren=true;return true;
}
bool UumuuBattle::bossDeath(){
    if(stage_!=Stage::Active)return false;
    stage_=Stage::EndPause;timer_=uumuu_data::battleEndWait;return true;
}
bool UumuuBattle::onDeathEvents(const UumuuDeathEvents& events){
    return events.bossDeath&&bossDeath();
}
void UumuuBattle::fixedUpdate(float dt){
    if(stage_!=Stage::EndPause||!std::isfinite(dt)||dt<=0)return;
    timer_-=dt;
    if(timer_<=0){stage_=Stage::Complete;events_.openGates=true;events_.deactivateChildren=true;}
}
void UumuuQuirrel::reset(){
    timer_=uumuu_data::quirrelInitialTimer;deadline_=uumuu_data::quirrelInitialTime;
    arrived_=uumuu_data::quirrelInitiallyArrived;events_={};
}
void UumuuQuirrel::idleUpdate(float dt){
    if(std::isfinite(dt)&&dt>0)timer_+=dt*uumuu_data::quirrelIdleTimerRate;
}
UumuuQuirrelDecision UumuuQuirrel::check(){
    // PlayMaker action order: FloatCompare, BoolTest, then targeted WATCH.
    if(timer_>=deadline_)return UumuuQuirrelDecision::Call;
    if(!arrived_)return UumuuQuirrelDecision::Idle;
    events_.watch=true;return UumuuQuirrelDecision::Waiting;
}
void UumuuQuirrel::setTimer(EnemyRandom random){
    timer_=uumuu_data::quirrelTimerReset;
    deadline_=random.range(uumuu_data::quirrelTimeMin,uumuu_data::quirrelTimeMax);
}
bool UumuuQuirrel::arrivePause(){
    events_.roamChanged=true;events_.roam=false;
    const bool previous=arrived_;arrived_=true;return previous;
}
void UumuuQuirrel::roam(){events_.roamChanged=true;events_.roam=true;}
}
