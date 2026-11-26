#include "watcher_knights.hpp"
#include "watcher_knights_data.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hk3ds {
namespace {
using namespace watcher_data;
float duration(const char* clip){for(const auto& c:clips)if(!std::strcmp(c.name,clip))return c.seconds;return 0;}
Rect localBox(Rect box,float scale){const float a=box.left*scale,b=box.right*scale;return {std::min(a,b),box.bottom,std::max(a,b),box.top};}
bool overlap(Rect a,Rect b){return a.left<b.right&&a.right>b.left&&a.bottom<b.top&&a.top>b.bottom;}
}
void WatcherEncounter::reset(){*this=WatcherEncounter{};}
bool WatcherEncounter::bind(CombatObject* objects,unsigned count,const AnimationBank* art,EnemyRandom random,
                            WatcherProgress progress,unsigned upgrades){
    if(!objects||count<capacity||upgrades>4)return false;
    reset();objects_=objects;art_=art;random_=random;progress_=progress;
    for(unsigned i=0;i<capacity;++i){
        auto& k=knights[i];k.position=spawns[i];k.scaleX=scales[i];k.presentation.reset(k.position);
        k.idleMin=idleMin;k.idleMax=idleMax;
        receivers_[i]={this,i};auto& o=objects_[i];o={};o.name="Watcher Knight";
        o.layer=11;o.hasHealth=true;o.receiver=&receivers_[i];o.onHit=receive;
        o.health.hp=hpByNail[upgrades];o.health.enemyType=9;o.health.invincible=true;
        enter(i,progress.battleActivated?WatcherPhase::Dead:WatcherPhase::Rest);
    }
    if(progress.battleActivated){stage_=Stage::Complete;remaining_=0;}
    return true;
}
Rect WatcherEncounter::bounds(unsigned i)const{return boxBounds(knights[i].position,localBox(body,knights[i].scaleX));}
Rect WatcherEncounter::arena()const{return {gates[0].right,std::min(gates[0].bottom,gates[1].bottom),gates[1].left,std::max(gates[0].top,gates[1].top)};}
const Rect& WatcherEncounter::gate(unsigned i){return gates[i<2?i:0];}
const Rect& WatcherEncounter::trigger(){return watcher_data::trigger;}
unsigned WatcherEncounter::awake()const{
    unsigned n=0;for(const auto& k:knights)n+=k.alive()&&k.phase>=WatcherPhase::RoarEnd;return n;
}
bool WatcherEncounter::inRange(unsigned i,Rect box,const Hero& hero)const{
    return overlap(boxBounds(knights[i].position,localBox(box,knights[i].scaleX)),bodyBounds(hero.position));
}
void WatcherEncounter::sync(unsigned i){objects_[i].bounds=bounds(i);objects_[i].colliderEnabled=knights[i].collider&&knights[i].alive();}
void WatcherEncounter::receive(void* context,const NailHit&,HitOutcome outcome){
    auto& r=*static_cast<Receiver*>(context);auto& e=*r.owner;auto& k=e.knights[r.index];
    if(!k.alive())return;
    if(outcome==HitOutcome::Damaged||outcome==HitOutcome::Killed)k.hitFlash=.1f;
    if(outcome==HitOutcome::Killed){
        e.enter(r.index,WatcherPhase::Dead);
        if(e.remaining_)--e.remaining_; // HealthManager.notifiedBattleScene, once.
    }
}
void WatcherEncounter::enter(unsigned i,WatcherPhase phase){
    auto& k=knights[i];auto& h=objects_[i].health;k.phase=phase;k.timer=0;
    const char* clip=nullptr;
    switch(phase){
    case WatcherPhase::Rest:clip="Rest";break;
    case WatcherPhase::BugsIn:k.timer=bugsTime;break;
    case WatcherPhase::CloudStop:k.timer=cloudTime;break;
    case WatcherPhase::BugsEnd:k.timer=bugsEndTime;break;
    case WatcherPhase::RoarStart:clip="Roar Start";k.collider=true;break;
    case WatcherPhase::Roar:clip="Roar";k.timer=roarTime;break;
    case WatcherPhase::RoarEnd:clip="RoarToIdle";k.timer=roarEndTime;k.contact=true;h.invincible=false;h.spellVulnerable=true;break;
    case WatcherPhase::Idle:clip="Idle";k.timer=random_.range(k.idleMin,k.idleMax);break;
    case WatcherPhase::Turn:clip="Turn";k.scaleX=-k.scaleX;break;
    case WatcherPhase::Run:
        clip="Run";k.velocity.x=-k.scaleX*runSpeed*random_.range(runModifyMin,runModifyMax);
        k.timer=random_.range(runWaitMin,runWaitMax);k.secondTimer=random_.range(runWaitMin,runWaitMax);break;
    case WatcherPhase::RollStart:clip="Roll Start";k.velocity={};break;
    case WatcherPhase::ChargeAntic:
        clip="Roll";k.charge=-k.scaleX*chargeSpeed;k.velocity={k.charge*-.25f,0};
        h.invincible=true;k.timer=chargeAnticTime;k.secondTimer=random_.range(chargeTimeMin,chargeTimeMax);break;
    case WatcherPhase::Charge:clip="Roll";k.timer=k.secondTimer;k.velocity.x=k.charge;break;
    case WatcherPhase::ChargeEnd:clip="Roll End";h.invincible=false;break;
    case WatcherPhase::ChargeRecover:clip="Idle";k.timer=chargeRecoverTime;break;
    case WatcherPhase::JumpAntic:clip="Roll";k.velocity={0,jumpAnticSpeed};h.invincible=true;break;
    case WatcherPhase::AnticAir:break;
    case WatcherPhase::JumpAir:clip="Roll";k.velocity={k.jumpX,jumpSpeed};break;
    case WatcherPhase::Bounce:clip="Roll";k.velocity={0,bounceSpeed};k.timer=bounceTime;break;
    case WatcherPhase::BounceAir:break;
    case WatcherPhase::Land:clip="Roll End";k.velocity={};h.invincible=false;break;
    case WatcherPhase::SlashAntic:clip="Slash Antic";k.velocity={};break;
    case WatcherPhase::Slash1:clip="Slash1";k.velocity={k.scaleX*slashSpeed,0};break;
    case WatcherPhase::Slash1Recover:clip="Slash1 Recover";k.velocity={};break;
    case WatcherPhase::Slash2:clip="Slash2";k.velocity={k.scaleX*slashSpeed,0};break;
    case WatcherPhase::Slash2Recover:clip="Slash2 Recover";k.velocity={};break;
    case WatcherPhase::Recover:clip="Idle";k.timer=recoverTime;break;
    case WatcherPhase::BackRoll:clip="Roll";k.velocity={k.scaleX*chargeSpeed,0};k.timer=backRollTime;h.invincible=true;break;
    case WatcherPhase::BackRollEnd:clip="Roll End";k.velocity={};h.invincible=false;break;
    case WatcherPhase::Dead:k.velocity={};k.contact=k.collider=false;break;
    }
    if(clip){
        char name[48];std::snprintf(name,sizeof(name),"Watcher/%s",clip);
        k.animation.play(art_?art_->require(name):nullptr,0,true);
        if(k.timer==0)k.timer=duration(clip);
    }
    sync(i);
}
void WatcherEncounter::idle(unsigned i,bool retime){
    auto& k=knights[i];
    if(retime){k.doubleAttack=awake()>1;k.idleMin=k.doubleAttack?groupIdleMin:soloIdleMin;k.idleMax=k.doubleAttack?groupIdleMax:soloIdleMax;}
    k.velocity={};enter(i,WatcherPhase::Idle);
}
void WatcherEncounter::rollStart(unsigned i,unsigned attack,const Hero& hero){
    if(attack<2&&!inRange(i,attackRange,hero)){enter(i,WatcherPhase::Run);return;}
    knights[i].attack=attack;++knights[i].attacks;enter(i,WatcherPhase::RollStart);
}
void WatcherEncounter::choice(unsigned i,const Hero& hero,bool near){
    auto& k=knights[i];
    const auto* weights=near?(k.doubleAttack?multiWeights:closeWeights):moveWeights;
    float sum=0;
    for(unsigned n=0;n<4;++n)if(k.repeats[n]<repeatMax[n])sum+=weights[n];
    float value=random_.range(0,sum);unsigned selected=2;
    for(unsigned n=0;n<4;++n)if(k.repeats[n]<repeatMax[n]&&weights[n]>0){selected=n;value-=weights[n];if(value<0)break;}
    const auto repeated=k.repeats[selected]+1;k.repeats.fill(0);k.repeats[selected]=repeated;
    if(selected==2)enter(i,near?WatcherPhase::SlashAntic:WatcherPhase::Run);else rollStart(i,selected,hero);
}
void WatcherEncounter::wake(unsigned i){
    current_=i;
    if(i==2&&progress_.watcherChandelier){enter(i,WatcherPhase::Dead);--remaining_;wake(i+1);return;}
    enter(i,WatcherPhase::BugsIn);stage_=Stage::Waiting;timer_=i==0?firstTimeout:0;
}
void WatcherEncounter::battle(float dt,Hero& hero){
    switch(stage_){
    case Stage::Idle:if(overlap(bodyBounds(hero.position),watcher_data::trigger)){stage_=Stage::Close;timer_=closeTime;}break;
    case Stage::Close:timer_-=dt;if(timer_<=0)wake(0);break;
    case Stage::Waiting:{
        constexpr unsigned threshold[]={5,5,4,3,2,0};timer_-=dt;
        if(current_==5&&remaining_==0){
            journal_=true;progress_.battleActivated=true;stage_=Stage::VictoryPause;timer_=victoryPause;
            if(onProgress)onProgress(progressContext,progress_);
        }else if(remaining_<=threshold[current_]){stage_=Stage::Delay;timer_=wakeDelay;}
        else if(current_==0&&timer_<=0)wake(1);
        break;}
    case Stage::Delay:timer_-=dt;if(timer_<=0)wake(current_+1);break;
    case Stage::VictoryPause:timer_-=dt;if(timer_<=0){stage_=Stage::MusicEnd;timer_=musicEndTime;}break;
    case Stage::MusicEnd:timer_-=dt;if(timer_<=0)stage_=Stage::Complete;break;
    case Stage::Complete:break;
    }
}
void WatcherEncounter::fixedUpdate(float dt,Hero& hero,const World& world){
    if(!active()||!std::isfinite(dt)||dt<=0)return;
    battle(dt,hero);
    for(unsigned i=0;i<capacity;++i){
        auto& k=knights[i];PresentationPosition::Step step(k.presentation,k.position);
        if(!k.alive()||k.phase<WatcherPhase::RoarStart)continue;
        k.timer-=dt;k.hitFlash=std::max(0.f,k.hitFlash-dt);
        const auto phase=k.phase;
        if(phase==WatcherPhase::ChargeEnd||phase==WatcherPhase::ChargeRecover){k.velocity.x*=chargeDeceleration;if(std::abs(k.velocity.x)<.001f)k.velocity.x=0;}
        if(phase==WatcherPhase::Charge)k.velocity.x=k.charge;
        k.velocity.y-=gravity*dt;
        const auto contact=moveBox(k.position,k.velocity,localBox(body,k.scaleX),dt,world);
        const bool wall=contact.left||contact.right;
        switch(phase){
        case WatcherPhase::RoarStart:if(k.timer<=0)enter(i,WatcherPhase::Roar);break;
        case WatcherPhase::Roar:if(k.timer<=0)enter(i,WatcherPhase::RoarEnd);break;
        case WatcherPhase::RoarEnd:if(k.timer<=0)idle(i,true);break;
        case WatcherPhase::Idle:
            if((hero.position.x>k.position.x)!=(k.scaleX<0))enter(i,WatcherPhase::Turn);
            else if(inRange(i,slashRange,hero))choice(i,hero,true);
            else if(k.timer<=0){if(k.doubleAttack&&!inRange(i,attackRange,hero))enter(i,WatcherPhase::Run);else choice(i,hero,false);}
            break;
        case WatcherPhase::Turn:if(k.timer<=0)idle(i,false);break;
        case WatcherPhase::Run:
            k.secondTimer-=dt;
            if(inRange(i,attackRange,hero)&&(hero.position.x>k.position.x)!=(k.scaleX<0))rollStart(i,1,hero);
            else if(inRange(i,slashRange,hero)){++k.attacks;enter(i,WatcherPhase::SlashAntic);}
            else if(k.timer<=0&&(!k.doubleAttack||inRange(i,attackRange,hero)))rollStart(i,0,hero);
            else if(k.secondTimer<=0)rollStart(i,1,hero);
            break;
        case WatcherPhase::RollStart:
            if(k.timer<=0){
                if(k.attack==0)enter(i,WatcherPhase::ChargeAntic);
                else if(k.attack==1){k.jumpX=std::clamp((hero.position.x-k.position.x)*jumpAimFactor+random_.range(jumpJitterMin,jumpJitterMax),jumpClampMin,jumpClampMax);enter(i,WatcherPhase::JumpAntic);}
                else enter(i,WatcherPhase::BackRoll);
            }break;
        case WatcherPhase::ChargeAntic:if(k.timer<=0)enter(i,WatcherPhase::Charge);break;
        case WatcherPhase::Charge:
            if(wall){k.scaleX=-k.scaleX;k.charge*=wallSpeedFactor;k.timer-=wallTimeLoss;}
            if(k.timer<=0)enter(i,WatcherPhase::ChargeEnd);
            break;
        case WatcherPhase::ChargeEnd:if(k.timer<=0)enter(i,WatcherPhase::ChargeRecover);break;
        case WatcherPhase::ChargeRecover:if(k.timer<=0)idle(i,true);break;
        case WatcherPhase::JumpAntic:enter(i,WatcherPhase::AnticAir);break;
        case WatcherPhase::AnticAir:if(contact.ground)enter(i,WatcherPhase::JumpAir);break;
        case WatcherPhase::JumpAir:
            if(wall){k.jumpX=-k.jumpX;k.velocity={k.jumpX,0};}
            if(contact.ground)enter(i,WatcherPhase::Bounce);
            break;
        case WatcherPhase::Bounce:if(k.timer<=0)enter(i,WatcherPhase::BounceAir);break;
        case WatcherPhase::BounceAir:if(contact.ground)enter(i,WatcherPhase::Land);break;
        case WatcherPhase::Land:if(k.timer<=0)enter(i,WatcherPhase::ChargeRecover);break;
        case WatcherPhase::SlashAntic:if(k.timer<=0)enter(i,WatcherPhase::Slash1);break;
        case WatcherPhase::Slash1:if(k.timer<=0)enter(i,WatcherPhase::Slash1Recover);break;
        case WatcherPhase::Slash1Recover:if(k.timer<=0)enter(i,WatcherPhase::Slash2);break;
        case WatcherPhase::Slash2:if(k.timer<=0)enter(i,WatcherPhase::Slash2Recover);break;
        case WatcherPhase::Slash2Recover:if(k.timer<=0)enter(i,WatcherPhase::Recover);break;
        case WatcherPhase::Recover:case WatcherPhase::BackRollEnd:if(k.timer<=0)idle(i,true);break;
        case WatcherPhase::BackRoll:if(k.timer<=0)enter(i,WatcherPhase::BackRollEnd);break;
        default:break;
        }
        sync(i);
        if(k.contact&&overlap(bounds(i),bodyBounds(hero.position)))hero.takeDamage(1,k.position.x<hero.position.x?-1:1);
        const auto slash=k.phase==WatcherPhase::Slash1?1:k.phase==WatcherPhase::Slash2?2:0;
        if(slash){
            std::array<Vec2,7> polygon{};const auto* points=slash==1?slash1:slash2;const unsigned count=slash==1?7:5;
            for(unsigned n=0;n<count;++n)polygon[n]={k.position.x+points[n].x*k.scaleX,k.position.y+points[n].y};
            if(polygonOverlapsBox(polygon.data(),count,bodyBounds(hero.position)))hero.takeDamage(1,k.position.x<hero.position.x?-1:1);
        }
    }
    // Waking particles have no physics; their three Wait states still consume
    // scaled fixed time. They never expose a hittable collider early.
    for(unsigned i=0;i<capacity;++i){auto& k=knights[i];
        if(k.phase<WatcherPhase::BugsIn||k.phase>WatcherPhase::BugsEnd)continue;
        k.timer-=dt;if(k.timer<=0)enter(i,k.phase==WatcherPhase::BugsIn?WatcherPhase::CloudStop:k.phase==WatcherPhase::CloudStop?WatcherPhase::BugsEnd:WatcherPhase::RoarStart);
    }
}
void WatcherEncounter::update(float dt){if(active()&&std::isfinite(dt)&&dt>0)for(auto& k:knights)if(k.alive())k.animation.advance(dt);}
}
