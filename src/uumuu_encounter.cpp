#include "uumuu_encounter.hpp"
#include "uumuu_data.hpp"
#include "uumuu_zap_data.hpp"
#include "uumuu_room_data.hpp"
#include <cmath>

namespace hk3ds {
namespace {
bool finite(Vec2 p){return std::isfinite(p.x)&&std::isfinite(p.y);}
bool validDt(float dt){return std::isfinite(dt)&&dt>0;}
// Consecutive chase spawns are at least zapWait apart, even before the first
// pause/recovery/idle waits. A complete burst fits without overwriting a zap.
static_assert(UumuuEncounter::zapCapacity*uumuu_attack_data::zapWait>=
    uumuu_zap_data::anticWait+uumuu_zap_data::activeWait+uumuu_zap_data::endPause,
    "Uumuu native zap storage must cover the authored overlapping lifetimes");
static_assert(sizeof(UumuuEncounter)<=2048,"Uumuu coordinator exceeded its fixed-state estimate");
}
bool UumuuEncounter::archiveOverlayRequired(std::string_view sceneName,bool defeated){
    return sceneName==uumuu_room_data::roomName&&defeated==uumuu_room_data::loadWhenDefeated;
}
bool UumuuEncounter::bindArchiveRoom(CombatObject& object,std::string_view sceneName,
                                     EnemyRandom random,bool defeated){
    if(!archiveOverlayRequired(sceneName,defeated))return false;
    return bind(object,{uumuu_room_data::bossX,uumuu_room_data::bossY},random,defeated);
}
bool UumuuEncounter::bind(CombatObject& object,Vec2 position,EnemyRandom random,bool defeated){
    if(!finite(position)||(&object!=object_&&(object.receiver||object.onHit)))return false;
    if((object.acceptedHitReceiver||object.onAcceptedHit)&&
       (object.acceptedHitReceiver!=this||object.onAcceptedHit!=acceptedHit))return false;
    reset();
    if(!shell_.bind(object))return false;
    object_=&object;random_=random;motion_.bind(position);battle_.bind(defeated);dispatchBattle();return true;
}
bool UumuuEncounter::bindArchiveRoom(CombatObject& object,std::string_view scene,
                                    EnemyRandom random,UumuuProgress& progress){
    if(!bindArchiveRoom(object,scene,random,progress.defeatedMegaJelly))return false;
    progress_=&progress;return true;
}
bool UumuuEncounter::bindHitAdapter(UumuuHitAdapter adapter){
    if(!object_||!adapter.capture)return false;
    if((object_->acceptedHitReceiver||object_->onAcceptedHit)&&
       (object_->acceptedHitReceiver!=this||object_->onAcceptedHit!=acceptedHit))return false;
    hitAdapter_=adapter;object_->acceptedHitReceiver=this;object_->onAcceptedHit=acceptedHit;
    return true;
}
bool UumuuEncounter::bindBody(UumuuBodyActor& body){
    if(!object_||!body.bound()||!body.enter(UumuuBodyAnimation::Idle))return false;
    body_=&body;return true;
}
bool UumuuEncounter::bindGates(UumuuGates& gates){
    if(!object_||gates_||battle_.encountered()||battle_.completed())return false;
    for(unsigned i=0;i<UumuuGates::capacity;++i)
        if(gates.phase(i)==UumuuGatePhase::Absent||gates.destructionPending(i))return false;
    gates_=&gates;return true;
}
bool UumuuEncounter::bindBodyColliders(){
    return object_&&colliders_.bind(*object_,motion_.position())&&syncBodyColliders();
}
bool UumuuEncounter::syncBodyColliders(){
    return colliders_.sync(motion_.position(),wake_.collidersEnabled());
}
void UumuuEncounter::acceptedHit(void* context,const NailHit& hit,const NailContact* contact){
    auto& self=*static_cast<UumuuEncounter*>(context);
    UumuuRecoilFrame frame;
    if(!self.hitAdapter_.capture||
       !self.hitAdapter_.capture(self.hitAdapter_.context,hit,contact,frame)||
       !frame.body||!frame.transform||!frame.world)return;
    self.recoil_.begin(hit.direction,frame.magnitude,frame.collider,
                       *frame.body,*frame.transform,*frame.world,
                       self.hitAdapter_.context,self.hitAdapter_.recoilStarted);
}
void UumuuEncounter::reset(){
    body_=nullptr;gates_=nullptr;battleEvents_={};
    if(object_&&object_->acceptedHitReceiver==this&&object_->onAcceptedHit==acceptedHit){
        object_->acceptedHitReceiver=nullptr;object_->onAcceptedHit=nullptr;
    }
    hitAdapter_={};
    colliders_.reset();shell_.reset();object_=nullptr;progress_=nullptr;random_={};battle_.bind(false);quirrel_.reset();attacks_.reset();
    motion_=UumuuMotion{};recoil_=UumuuRecoil{};multizaps_=UumuuMultizaps{};
    for(auto& zap:zaps_)zap.deactivate();
    death_.reset();wake_.reset();attackEvents_={};deathEvents_={};poolExhausted_=false;
    arrivalReceived_=slashAnticReceived_=slashPoseReceived_=false;
    arrivalPhase_=UumuuArrivalPhase::Inactive;arrivalTimer_=0;arrivalEvents_={};
}
bool UumuuEncounter::arenaEntered(){
    if(!object_||!battle_.arenaEntered())return false;
    dispatchBattle();
    wake_.battleStart();
    if(progress_)progress_->encounteredMegaJelly=true;
    return true;
}
bool UumuuEncounter::wakeTweenUpdate(float dt){
    if(!object_||!battle_.locked())return false;
    auto position=motion_.position();
    if(!wake_.tweenUpdate(dt,position))return false;
    if(!motion_.place(position))return false;
    syncBodyColliders();return true;
}
bool UumuuEncounter::arrivePause(Vec2 hero){
    if(!object_||!finite(hero)||attacks_.phase()!=UumuuAttackPhase::Quirrel||arrivalReceived_)return false;
    arrivalReceived_=true;motion_.enter(UumuuMotionMode::Chase,hero);
    const bool subsequent=quirrel_.arrivePause();
    arrivalPhase_=subsequent?UumuuArrivalPhase::AttackPause:UumuuArrivalPhase::FirstPause;
    arrivalTimer_=subsequent?uumuu_data::quirrelAttackPauseWait:uumuu_data::quirrelArrivePauseWait;
    return subsequent;
}
bool UumuuEncounter::quirrelLeaveAnimationFinished(Vec2 hero){
    if(!object_||!finite(hero)||arrivalPhase_!=UumuuArrivalPhase::Leave||
       shell_.phase()!=UumuuShellPhase::Guarded)return false;
    arrivalPhase_=UumuuArrivalPhase::SlashPause;arrivalTimer_=uumuu_data::quirrelSlashPauseWait;
    motion_.enter(UumuuMotionMode::Chase,hero);arrivalEvents_.hide=true;return true;
}
bool UumuuEncounter::slashAntic(Vec2 hero){
    if(!object_||!finite(hero)||attacks_.phase()!=UumuuAttackPhase::Quirrel||arrivalPhase_!=UumuuArrivalPhase::Ready||
       slashAnticReceived_||shell_.phase()!=UumuuShellPhase::Guarded)return false;
    slashAnticReceived_=true;arrivalEvents_.slashAntic=true;
    return motion_.enter(UumuuMotionMode::Dampen,hero);
}
bool UumuuEncounter::prepareSlashAntic(float rotation,int scaleRoll,UumuuSlashPose& pose){
    if(!object_||arrivalPhase_!=UumuuArrivalPhase::Ready||!slashAnticReceived_||slashPoseReceived_||
       shell_.phase()!=UumuuShellPhase::Guarded||!std::isfinite(rotation)||
       rotation<uumuu_data::quirrelSlashRotationMin||rotation>uumuu_data::quirrelSlashRotationMax||
       scaleRoll<0||scaleRoll>=100)return false;
    // RandomFloatEither.OnEnter uses the integer overload: Range(0,100)<50.
    pose={rotation,scaleRoll<50?uumuu_data::quirrelSlashScale1:uumuu_data::quirrelSlashScale2};
    slashPoseReceived_=true;return true;
}
bool UumuuEncounter::beginSlash(Vec2 hero){
    if(!object_||!finite(hero)||attacks_.phase()!=UumuuAttackPhase::Quirrel||arrivalPhase_!=UumuuArrivalPhase::Ready||
       !slashPoseReceived_||!shell_.beginSlash())return false;
    arrivalPhase_=UumuuArrivalPhase::Slashed;
    if(body_)body_->enter(UumuuBodyAnimation::Wound);
    motion_.enter(UumuuMotionMode::Dampen,hero);return true;
}
bool UumuuEncounter::slashAnimationFinished(Vec2 hero){
    if(!object_||!finite(hero)||!shell_.slashAnimationFinished())return false;
    motion_.enter(UumuuMotionMode::Wounded,hero);recoil_.onShellPhase(shell_.phase());return true;
}
bool UumuuEncounter::afterQuirrelRecovery(Vec2 hero){
    if(!object_||!finite(hero)||arrivalPhase_!=UumuuArrivalPhase::Slashed||shell_.phase()!=UumuuShellPhase::Guarded||
       !attacks_.afterQuirrelRecovery(quirrel_))return false;
    dispatchAttack(hero);return true;
}
void UumuuEncounter::dispatchAttack(Vec2 hero){
    attackEvents_=attacks_.takeEvents();motion_.onAttackEvents(attackEvents_,hero);
    if(body_){
        if(attackEvents_.antic)body_->enter(UumuuBodyAnimation::Attack);
        if(attackEvents_.recover)body_->enter(UumuuBodyAnimation::Recover);
    }
    if(attackEvents_.callQuirrel){
        arrivalReceived_=slashAnticReceived_=slashPoseReceived_=false;arrivalPhase_=UumuuArrivalPhase::Inactive;
        arrivalTimer_=0;arrivalEvents_={};
    }
    multizaps_.onAttackEvents(attackEvents_,random_);
    if(!attackEvents_.spawnZap)return;
    for(auto& zap:zaps_)if(zap.phase()==UumuuZapPhase::Inactive){
        zap.onAttackEvents(attackEvents_);return;
    }
    // A missed pool callback is observable; never overwrite a live hazard.
    poolExhausted_=true;
}
void UumuuEncounter::arrivalUpdate(float dt,Vec2 hero){
    switch(arrivalPhase_){
    case UumuuArrivalPhase::FirstPause:case UumuuArrivalPhase::Arrive:
    case UumuuArrivalPhase::SlashPause:case UumuuArrivalPhase::AttackPause:break;
    default:return;
    }
    arrivalTimer_-=dt;if(arrivalTimer_>0)return;
    // Each new source Wait starts here; an oversized callback cannot consume it.
    if(arrivalPhase_==UumuuArrivalPhase::FirstPause){
        const bool right=hero.x>uumuu_data::quirrelHeroMidX,top=hero.y>uumuu_data::quirrelHeroMidY;
        if(top){
            arrivalEvents_.position=right?Vec2{uumuu_data::quirrelTRX,uumuu_data::quirrelTRY}:
                                          Vec2{uumuu_data::quirrelTLX,uumuu_data::quirrelTLY};
            arrivalEvents_.scaleX=right?uumuu_data::quirrelTRScale:uumuu_data::quirrelTLScale;
        }else{
            arrivalEvents_.position=right?Vec2{uumuu_data::quirrelBRX,uumuu_data::quirrelBRY}:
                                          Vec2{uumuu_data::quirrelBLX,uumuu_data::quirrelBLY};
            arrivalEvents_.scaleX=right?uumuu_data::quirrelBRScale:uumuu_data::quirrelBLScale;
        }
        arrivalPhase_=UumuuArrivalPhase::Arrive;arrivalTimer_=uumuu_data::quirrelArriveWait;
        arrivalEvents_.arrive=true;motion_.enter(UumuuMotionMode::Chase,hero);
    }else if(arrivalPhase_==UumuuArrivalPhase::Arrive){
        arrivalPhase_=UumuuArrivalPhase::Leave;arrivalEvents_.leave=true;
        motion_.enter(UumuuMotionMode::Chase,hero);
    }else{
        arrivalPhase_=UumuuArrivalPhase::Ready;slashAntic(hero);
    }
}
void UumuuEncounter::controlUpdate(float dt,Vec2 hero){
    if(!object_||!validDt(dt)||!finite(hero)||battle_.completed()||shell_.phase()==UumuuShellPhase::Dead)return;
    if(wake_.phase()!=UumuuWakePhase::Ready){
        wake_.controlUpdate(dt);
        syncBodyColliders();
        if(wake_.phase()==UumuuWakePhase::Ready&&attacks_.start(random_))dispatchAttack(hero);
        return;
    }
    arrivalUpdate(dt,hero);
    const auto previous=shell_.phase();shell_.fixedUpdate(dt);
    if(shell_.phase()!=previous){
        recoil_.onShellPhase(shell_.phase());
        if(shell_.phase()==UumuuShellPhase::RecoverAntic){
            motion_.enter(UumuuMotionMode::Dampen,hero);
            if(body_)body_->enter(UumuuBodyAnimation::RecoverAntic);
        }else if(shell_.phase()==UumuuShellPhase::Recover){
            motion_.enter(UumuuMotionMode::Coast,hero);
            if(body_)body_->enter(UumuuBodyAnimation::Recover);
        }
    }
    // Recover FINISHED -> Quirrel Roam -> Quirrel? is synchronous. The new
    // Idle/Quirrel state must not consume this callback's elapsed wait again.
    const bool recovered=previous==UumuuShellPhase::Recover&&shell_.phase()==UumuuShellPhase::Guarded;
    if(recovered)attacks_.afterQuirrelRecovery(quirrel_);
    else attacks_.fixedUpdate(dt,hero,quirrel_);
    dispatchAttack(hero);
}
void UumuuEncounter::lightningUpdate(float dt){
    if(!object_||!validDt(dt))return;
    multizaps_.fixedUpdate(dt);for(auto& zap:zaps_)zap.fixedUpdate(dt);
}
void UumuuEncounter::contactLightning(Hero& hero,Rect box)const{
    if(!object_)return;
    multizaps_.contactHero(hero,box);for(const auto& zap:zaps_)zap.contactHero(hero,box);
}
void UumuuEncounter::battleUpdate(float dt){
    if(!object_)return;
    battle_.fixedUpdate(dt);dispatchBattle();
}
void UumuuEncounter::dispatchBattle(){
    const auto e=battle_.takeEvents();
    if(gates_)gates_->battleEvents(e);
    battleEvents_.encounteredMegaJelly|=e.encounteredMegaJelly;
    battleEvents_.battleStart|=e.battleStart;
    battleEvents_.closeGates|=e.closeGates;
    battleEvents_.openGates|=e.openGates;
    battleEvents_.quickOpenGates|=e.quickOpenGates;
    battleEvents_.activateChildren|=e.activateChildren;
    battleEvents_.deactivateChildren|=e.deactivateChildren;
}
bool UumuuEncounter::corpseActivated(bool bossScene){
    if(!object_||battle_.completed()||!death_.activate(bossScene))return false;
    dispatchDeath();return true;
}
void UumuuEncounter::corpseUpdate(float dt){
    if(!object_||!validDt(dt))return;
    death_.fixedUpdate(dt);dispatchDeath();
}
void UumuuEncounter::dispatchDeath(){
    deathEvents_=death_.takeEvents();
    if(progress_&&deathEvents_.defeatedMegaJelly)progress_->defeatedMegaJelly=true;
    battle_.onDeathEvents(deathEvents_);
    dispatchBattle();
}
unsigned UumuuEncounter::activeZaps()const{
    unsigned count=0;for(const auto& zap:zaps_)if(zap.phase()!=UumuuZapPhase::Inactive)++count;
    return count;
}
}
