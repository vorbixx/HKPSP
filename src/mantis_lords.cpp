#include "mantis_lords.hpp"
#include "area_title_data.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace hk3ds {
namespace {
// Original level180 Mantis Lord FSM values; source provenance in the task note.
constexpr float dashSpeed=55,stabSpeed=-65;
constexpr float dashLeft=22.25f,dashRight=37.65f;
// level180 throne GameObjects3,2,25 (central Lord fights first).
constexpr Vec2 seats[]={{30.2199993f,17.1523018f},{26.2584381f,15.6023026f},{34.4399986f,15.6723022f}};
constexpr float seatFacing[]={-1,1,-1};
// level180 Challenge Prompt transform100 and BoxCollider2D182, parent90 identity.
constexpr Rect challengeRegion={25.5146728f,5.8723025f,35.1577859f,6.8723025f};
constexpr unsigned soloRepeat[]={2,3,2},soloMissed[]={5,5,5};
constexpr unsigned pairRepeat[]={2,2,2,2,2,1,1},pairMissed[]={10,10,10,10,10,12,12};
bool highThrow(const Hero& hero){return hero.position.y>19||hero.position.x<20.16f||hero.position.x>39.87f;}
}
unsigned MantisChoices::select(EnemyRandom random,unsigned count,const unsigned* repeatMax,const unsigned* missedMax){
    count=std::min(count,unsigned(repeats.size()));
    if(!count)return 0;
    unsigned selected=count;
    // SendRandomEventV3 scans in authored order; the last overdue event wins.
    for(unsigned i=0;i<count;++i)if(missed[i]>=missedMax[i])selected=i;
    const bool forced=selected<count;
    if(!forced){
        unsigned options[7],available=0;
        for(unsigned i=0;i<count;++i)if(repeats[i]<repeatMax[i])options[available++]=i;
        if(!available){repeats={};for(unsigned i=0;i<count;++i)options[available++]=i;}
        // Equal source weights permit sampling eligible events directly,
        // avoiding the original's bounded rejection loop on ARM.
        selected=options[std::min(unsigned(random.range(0,float(available))),available-1)];
    }
    for(unsigned i=0;i<count;++i){
        repeats[i]=i==selected?(forced?1:std::min(255u,unsigned(repeats[i])+1)):0;
        missed[i]=i==selected?0:std::min(255u,unsigned(missed[i])+1);
    }
    return selected;
}
float MantisLords::play(AnimationPlayer& a,const char* name){
    if(!art_)return .3f;
    char key[96];std::snprintf(key,sizeof(key),"Lord/%s",name);
    a.play(art_->require(key),0,true);
    return a.clip&&a.clip->fps>0?float(a.clip->count)/a.clip->fps:.3f;
}
bool MantisLords::bind(CombatObject* objects,unsigned count,const AnimationBank* art,EnemyRandom random,bool defeated){
    if(!objects||count<capacity)return false;
    reset();objects_=objects;art_=art;random_=random;
    for(unsigned i=0;i<capacity;++i){
        receivers_[i]={this,i};auto& o=objects_[i];o={};
        o.name="Mantis Lord";o.layer=11;o.hasHealth=true;o.receiver=&receivers_[i];o.onHit=receive;
        o.health.hp=i?160:210;o.health.enemyType=9;o.health.invincible=true;
        auto& k=lords[i];k.position=seats[i];k.scaleX=seatFacing[i];k.defeated=defeated;
        k.presentation.reset(k.position);enter(i,defeated?MantisPhase::Bow:MantisPhase::Seated);
    }
    if(defeated)stage_=Stage::Complete;
    return true;
}
unsigned MantisLords::remaining()const{unsigned n=0;for(const auto& k:lords)n+=!k.defeated;return n;}
bool MantisLords::challengeAvailable(Vec2 p)const{
    return active()&&stage_==Stage::Rest&&boxSensorOverlap(bodyBounds(p),challengeRegion);
}
// Challenge Prompt / Can Talk?: Can Input, On Ground, no attack or dash.
bool MantisLords::challenge(Hero& hero){
    if(!challengeAvailable(hero.position)||!hero.inputEnabled||!hero.controlEnabled||
       !hero.contacts.ground||hero.nail.active||hero.dashing)return false;
    stage_=Stage::Intro;introAge_=0;
    hero.controlEnabled=false;hero.velocity={};
    heroFacing_=hero.facingRight;
    hero.facingRight=hero.position.x<seats[0].x;
    return true;
}
const char* MantisLords::heroClip()const{
    if(stage_!=Stage::Intro||introAge_>=regainTime)return nullptr;
    return introAge_<endTime?"Challenge Start":"Challenge End";
}
float MantisLords::titleAlpha()const{
    if(!active()||!locked()||introAge_<cageTime)return 0;
    // Area Title Control: Visited=true selects the short VisitPause path.
    const float t=introAge_-cageTime;
    if(t<0)return 0;
    return t<areatitledata::appear?std::min(1.f,t/areatitledata::fadeIn):
        std::max(0.f,1.f-(t-areatitledata::appear)/areatitledata::fadeOut);
}
AnimationPlayer MantisLords::heroAnimation()const{
    AnimationPlayer result;
    if(const auto* name=heroClip();name&&art_){
        char key[64];std::snprintf(key,sizeof(key),"Hero/%s",name);
        result.play(art_->require(key));
        result.advance(introAge_-(introAge_>=endTime?endTime:0));
    }
    return result;
}
float MantisLords::gateOffset()const{
    if(!sceneryVisible())return 0;
    if(outroAge_<0)return 19.f*std::max(0.f,1.f-(introAge_-cageTime)/.5f);
    // Cage Control: Rise 1 moves +2 in 2s, then at 1.5s Rise 2 moves +19 in 2s.
    // approximation: retain both displacement curves across the state boundary;
    // the export omits iTween replacement semantics (at most .03125 units).
    const float rise1=std::min(1.f,outroAge_/2.f);
    const float rise2=std::clamp((outroAge_-1.5f)/2.f,0.f,1.f);
    const float remaining=1.f-rise1;
    return 2.f*(1.f-remaining*remaining*remaining)+19.f*rise2*rise2*rise2;
}
const char* MantisLords::floorClip(unsigned side)const{
    if(!active())return nullptr;
    // Floor Control hides its mesh after Retract (3 frames at 12 fps).
    if(floorRetracted(side))return locked()&&floorAge()<3.f/12?"Floor/Retract":nullptr;
    // Floor + Cage sends EXTEND after Return 2 (3.75s) and Bow (3s).
    if(outroAge_>=0&&floorExtendAge_[side]<4.f/12)return "Floor/Extend";
    return "Floor/Idle";
}
Rect MantisLords::bounds(unsigned i)const{
    const float scale=lords[i].scaleX;
    return boxBounds(lords[i].position,{std::min(-.765625f*scale,.578125f*scale),-2.90625f,
        std::max(-.765625f*scale,.578125f*scale),-.515625f});
}
void MantisLords::sync(unsigned i){
    const auto& k=lords[i];auto& o=objects_[i];o.bounds=bounds(i);
    o.colliderEnabled=k.fighting&&!k.defeated&&k.visible()&&
        k.phase!=MantisPhase::Leave&&k.phase!=MantisPhase::DashLeave&&
        k.phase!=MantisPhase::ThroneLeave;
    o.health.invincible=!o.colliderEnabled;o.health.spellVulnerable=o.colliderEnabled;
}
void MantisLords::receive(void* context,const NailHit&,HitOutcome outcome){
    auto& r=*static_cast<Receiver*>(context);auto& e=*r.owner;auto& k=e.lords[r.index];
    if(!k.fighting||k.defeated||outcome!=HitOutcome::Killed)return;
    k.defeated=true;k.fighting=false;e.enter(r.index,MantisPhase::DeathStart);e.sync(r.index);
}
void MantisLords::enter(unsigned i,MantisPhase phase){
    auto& k=lords[i];k.phase=phase;k.velocity={};k.timer=0;
    switch(phase){
    case MantisPhase::Seated:play(k.animation,"Throne Idle");break;
    case MantisPhase::ThroneStand:play(k.animation,"Throne Stand");break;
    case MantisPhase::ThroneLeave:k.timer=play(k.animation,"Throne Leave");break;
    case MantisPhase::StartPause:k.timer=.9f;break; // Start Pause .5 + Idle .4.
    case MantisPhase::Arrive:k.position={i==2?37.65f:22.25f,10.28f};k.scaleX=i==2?-1:1;k.presentation.reset(k.position);k.timer=.6f;play(k.animation,"Dash Arrive");break;
    case MantisPhase::Idle:k.timer=.4f;break;
    case MantisPhase::Scheduled:break;
    case MantisPhase::DashAntic:k.timer=play(k.animation,"Dash Antic");break;
    case MantisPhase::DashArrive:k.timer=play(k.animation,"Dash Arrive");break;
    case MantisPhase::Dash:k.velocity.x=k.scaleX*dashSpeed;play(k.animation,"Dash Attack");break;
    case MantisPhase::DashEnd:
        k.position.x=k.scaleX>0?dashRight:dashLeft;k.timer=play(k.animation,"Dash Recover");break;
    case MantisPhase::DashLeave:k.timer=play(k.animation,"Dash Leave");break;
    case MantisPhase::StabAntic:k.timer=play(k.animation,"Dstab Arrive");break;
    case MantisPhase::Stab:k.velocity.y=stabSpeed;play(k.animation,"Dstab Attack");break;
    case MantisPhase::Land:k.position.y=8.99f;k.timer=play(k.animation,"Dstab Land");break;
    case MantisPhase::StabLeave:k.timer=play(k.animation,"Dstab Leave");break;
    // Regular Wall Arrive has concurrent clip completion and a .6s event.
    // Keep Throwing? has an enabled CANCEL; its repeat actions are disabled.
    case MantisPhase::Wall:k.throwsRemaining=1;k.timer=std::min(.6f,play(k.animation,"Wall Arrive"));break;
    case MantisPhase::ThrowAntic:k.timer=play(k.animation,"Throw Antic");break;
    case MantisPhase::Throw:k.timer=play(k.animation,"Throw");launch(i);break;
    case MantisPhase::ThrowCooldown:k.timer=.5f;break;
    case MantisPhase::WallLeavePause:k.timer=.5f;break;
    case MantisPhase::WallLeave1:k.timer=play(k.animation,"Wall Leave 1");break;
    case MantisPhase::WallLeave2:k.timer=play(k.animation,"Wall Leave 2");break;
    case MantisPhase::AfterThrowPause:k.timer=.65f;break;
    // sharedassets180 animator85/86 starts clip33; the first corpse skips Stop Effect.
    case MantisPhase::DeathStart:k.timer=i?1.5f:1.f;play(k.animation,"Death New 1");break;
    case MantisPhase::Leave:k.timer=play(k.animation,"Death New 2");break;
    // First: hidden corpse Wait1 + throne Pause.25. Paired throne Return Pause
    // is 2.25s from lethal hit, after Init1 + Stop Effect.5 + departure3/16.
    case MantisPhase::ReturnPause:k.timer=i?2.25f-1.5f-3.f/16:1.25f;break;
    case MantisPhase::Wounded:play(k.animation,"Throne Wounded");break;
    case MantisPhase::Bow:play(k.animation,"Throne Bow");break;
    }
    sync(i);
}
void MantisLords::startMove(unsigned i,const Hero& hero,unsigned move){
    auto& k=lords[i];
    k.throwMode=0;
    const bool left=move==4?true:move==5?false:
        hero.position.x<29.98f?false:hero.position.x>30.18f?true:random_.range(0,1)<.5f;
    switch(move){
    case 0: // DASH (Dash Dir -> Dash R/L)
    case 4: // DASH R, explicitly sent by Battle Sub.
    case 5: // DASH L.
        k.position={left?22.25f:37.65f,10.28f};k.scaleX=left?1.f:-1.f;
        enter(i,MantisPhase::DashArrive);break;
    case 1: // DSTAB (Dstab Pos -> Dstab Arrive)
        k.position={std::clamp(hero.position.x+(random_.range(0,1)<.5f?random_.range(-4,4):0),21.5f,38.5f),hero.position.y<12?16.96f:19.2f};
        enter(i,MantisPhase::StabAntic);break;
    case 2: // THROW (Throw UD? -> Throw Pos)
        k.throwHigh=random_.range(0,1)<.5f;
        k.position={hero.position.x<22?41.77f:hero.position.x>37?18.2f:(random_.range(0,1)<.5f?18.2f:41.77f),
            k.throwHigh?random_.range(11.5f,12.5f):random_.range(9.5f,10.f)};
        k.scaleX=k.position.x<30?-1.f:1.f;
        enter(i,MantisPhase::Wall);break;
    case 3: // HIGH THROW (High Check LR -> Throw L/R)
        k.throwHigh=false;
        k.position={hero.position.x<22?41.77f:hero.position.x>37?18.2f:(random_.range(0,1)<.5f?18.2f:41.77f),std::clamp(hero.position.y-1.f,9.f,21.93f)};
        k.scaleX=k.position.x<30?-1.f:1.f;
        enter(i,MantisPhase::Wall);break;
    default: // NARROW L/R (6/7), WIDE L/R (8/9).
        k.throwMode=move<8?1:2;k.throwHigh=true;
        k.position={move%2==0?18.2f:41.77f,random_.range(14.5f,15.5f)};
        k.scaleX=move%2==0?-1.f:1.f;
        enter(i,MantisPhase::Wall);break;
    }
    k.presentation.reset(k.position);
}
void MantisLords::schedule(unsigned i,unsigned move,float delay,const Hero& hero){
    if(lords[i].defeated)return;
    if(delay<=0){startMove(i,hero,move);return;}
    enter(i,MantisPhase::Scheduled);lords[i].pendingMove=move;lords[i].timer=delay;
}
void MantisLords::choosePair(const Hero& hero){
    if(highThrow(hero)){schedule(1,3,.5f,hero);pairWait_=.5f;return;}
    unsigned pattern=pairChoices_.select(random_,7,pairRepeat,pairMissed);
    // Pincer Check rejects this attack outside its central safe interval.
    if(pattern==3&&(hero.position.x<27.24f||hero.position.x>33.35f)){
        pairChoices_.repeats[3]=2;pairChoices_.missed[3]=0;
        pattern=pairChoices_.select(random_,7,pairRepeat,pairMissed);
    }
    switch(pattern){
    case 0: // STAB DASH
        schedule(1,1,0,hero);schedule(2,0,.8f,hero);pairWait_=2.75f;break;
    case 1: // DASH STAB
        schedule(1,0,0,hero);schedule(2,1,1.5f,hero);pairWait_=2.75f;break;
    case 2: // DOUBLE DASH; the second direction is checked at dispatch time.
        secondDashLeft_=hero.position.x>30.f;secondDash_=true;
        schedule(1,secondDashLeft_?4:5,0,hero);
        schedule(2,secondDashLeft_?4:5,1.5f,hero);pairWait_=3.75f;break;
    case 3: // PINCER
        schedule(1,4,0,hero);schedule(2,5,0,hero);pairWait_=2;break;
    case 4: // DOUBLE STAB
        schedule(1,1,0,hero);schedule(2,1,.75f,hero);pairWait_=2.2f;break;
    case 5: // D THROW NARROW
    case 6: // D THROW WIDE
        schedule(1,pattern==5?6:8,.5f,hero);schedule(2,pattern==5?7:9,.5f,hero);pairWait_=4;break;
    }
}
void MantisLords::choose(unsigned i,const Hero& hero){
    const bool paired=stage_==Stage::Pair&&!lords[1].defeated&&!lords[2].defeated;
    if(paired){
        if(pairWait_<=0&&lords[1].phase==MantisPhase::Idle&&lords[2].phase==MantisPhase::Idle)choosePair(hero);
        return;
    }
    auto& k=lords[i];
    const unsigned move=i==0&&k.attack==0?0:highThrow(hero)?3:k.choices.select(random_,3,soloRepeat,soloMissed);
    ++k.attack;
    startMove(i,hero,move);
}
void MantisLords::launch(unsigned i){
    const auto& k=lords[i];
    for(auto& p:lances)if(!p.active){
        p={};p.active=true;p.direction=-k.scaleX;p.high=k.throwHigh;
        // level180 Throw Point transforms86/102/136 are relative to the Lord.
        p.position={k.position.x-2.01f*k.scaleX,k.position.y-1.97f};
        // Control / DU translates down .5 before starting the rising arc.
        if(!p.high)p.position.y-=.5f;
        p.force=k.throwMode==1?12.f:24.f;
        p.origin=p.position;p.velocity={p.direction*(k.throwMode==1?16.5f:33.f),k.throwMode?-2.2f:p.high?-1.1f:p.position.y>=19?2.f:1.1f};
        p.presentation.reset(p.position);play(p.animation,"Mantis Air Projectile");break;
    }
}
void MantisLords::fixedUpdate(float dt,Hero& hero){
    if(!active()||!std::isfinite(dt)||dt<=0)return;
    pairWait_-=dt;
    if(stage_==Stage::Complete&&outroAge_>=0)outroAge_+=dt;
    const bool up=hero.input.y>.5f;
    if(up&&!upHeld_)challenge(hero);
    upHeld_=up;
    if(locked())introAge_+=dt;
    if(stage_==Stage::Intro){
        if(introAge_<regainTime){hero.controlEnabled=false;hero.velocity.x=0;}
        // MLORD CHALLENGE reaches all three throne FSMs; the side Lords
        // hold Stand until MLORD START SUB, while the centre Lord leaves first.
        if(introAge_>=standTime)
            for(unsigned i=0;i<capacity;++i)
                if(lords[i].phase==MantisPhase::Seated)enter(i,MantisPhase::ThroneStand);
        if(introAge_>=regainTime&&lords[0].phase==MantisPhase::ThroneStand){
            hero.controlEnabled=true;hero.facingRight=heroFacing_;enter(0,MantisPhase::ThroneLeave);
        }
    }
    if(stage_==Stage::Solo&&lords[0].defeated){
        // Corpse First Init1 + Depart3/16 + Leave Pause1, then throne Pause.25
        // and Defeated Wait1 before MLORD START SUB.
        stage_=Stage::Intermission;timer_=1.f+3.f/16+1.f+.25f+1.f;
    }
    if(stage_==Stage::Intermission&&(timer_-=dt)<=0){
        stage_=Stage::Pair;
        // MLORD START SUB plays both throne departures. Battle Sub's Init
        // Pause waits .5s before choosing a move; combat actors stay hidden.
        pairWait_=.5f;
        for(unsigned i=1;i<capacity;++i){lords[i].fighting=true;enter(i,MantisPhase::ThroneLeave);}
    }
    if(stage_==Stage::Pair&&lords[1].defeated&&lords[2].defeated){stage_=Stage::Victory;timer_=6.75f;for(auto& p:lances)p.active=false;}
    if(stage_==Stage::Victory){
        timer_-=dt;
        if(timer_<=3&&lords[0].phase!=MantisPhase::Bow)
            for(unsigned i=0;i<capacity;++i)enter(i,MantisPhase::Bow);
        if(timer_<=0){stage_=Stage::Complete;outroAge_=0;}
    }
    if(stage_==Stage::Complete&&outroAge_>=0){
        // level180 Hero Check triggers GO6/22, BoxCollider2D178/180,
        // transformed through opening-floor parents 84/93. Floor Control
        // retries Extend each frame until its trigger is clear, then enables
        // both collider and mesh before playing the four-frame animation.
        constexpr Rect checks[]={{17.0307083f,3.8041837f,20.8930931f,6.059624f},
                                 {39.0569077f,3.8041837f,42.9192924f,6.059624f}};
        for(unsigned side=0;side<2;++side){
            auto& age=floorExtendAge_[side];
            if(age>=0)age+=dt;
            else if(!boxSensorOverlap(bodyBounds(hero.position),checks[side]))age=0;
        }
    }
    for(unsigned i=0;i<capacity;++i){
        auto& k=lords[i];PresentationPosition::Step pose(k.presentation,k.position);k.timer-=dt;
        k.position.x+=k.velocity.x*dt;k.position.y+=k.velocity.y*dt;
        switch(k.phase){
        case MantisPhase::Scheduled:
            if(k.timer<=0){
                if(i==2&&secondDash_){
                    if(secondDashLeft_&&hero.position.x<27.24f)secondDashLeft_=false;
                    else if(!secondDashLeft_&&hero.position.x>33.35f)secondDashLeft_=true;
                    k.pendingMove=secondDashLeft_?4:5;secondDash_=false;
                }
                startMove(i,hero,k.pendingMove);
            }
            break;
        case MantisPhase::ThroneLeave:
            if(k.timer<=0){
                if(i==0){stage_=Stage::Solo;enter(i,MantisPhase::StartPause);}
                else{enter(i,MantisPhase::Idle);k.timer=std::max(0.f,pairWait_);}
            }
            break;
        case MantisPhase::StartPause:if(k.timer<=0){k.fighting=true;choose(i,hero);}break;
        case MantisPhase::Seated:{
            // Sit Up FSM: ENTER raises the head; EXIT delays .1-.5 seconds.
            const bool inRange=boxSensorOverlap(bodyBounds(hero.position),challengeRegion);
            if(inRange){
                if(!k.throneRaised||k.lookExitPending)play(k.animation,"Throne Look Up");
                k.throneRaised=true;k.lookExitPending=false;
            }else if(k.throneRaised){
                if(!k.lookExitPending){k.timer=random_.range(.1f,.5f);k.lookExitPending=true;}
                else if(k.timer<=0){play(k.animation,"Throne Look Down");k.throneRaised=false;k.lookExitPending=false;}
            }
            break;
        }
        case MantisPhase::Arrive:if(k.timer<=0)enter(i,MantisPhase::Idle);break;
        case MantisPhase::Idle:if(k.fighting&&k.timer<=0)choose(i,hero);break;
        case MantisPhase::DashArrive:if(k.timer<=0)enter(i,MantisPhase::DashAntic);break;
        case MantisPhase::DashAntic:if(k.timer<=0)enter(i,MantisPhase::Dash);break;
        case MantisPhase::Dash:
            if((k.scaleX>0&&k.position.x>=dashRight)||(k.scaleX<0&&k.position.x<=dashLeft))
                enter(i,MantisPhase::DashEnd);
            break;
        case MantisPhase::DashEnd:if(k.timer<=0)enter(i,MantisPhase::DashLeave);break;
        case MantisPhase::Land:if(k.timer<=0)enter(i,MantisPhase::StabLeave);break;
        case MantisPhase::DashLeave:case MantisPhase::StabLeave:
            if(k.timer<=0)enter(i,MantisPhase::Idle);
            break;
        case MantisPhase::StabAntic:if(k.timer<=0)enter(i,MantisPhase::Stab);break;
    case MantisPhase::Stab:if(k.position.y<=8.99f)enter(i,MantisPhase::Land);break;
        case MantisPhase::Wall:if(k.timer<=0)enter(i,MantisPhase::ThrowAntic);break;
        case MantisPhase::ThrowAntic:if(k.timer<=0)enter(i,MantisPhase::Throw);break;
        case MantisPhase::Throw:if(k.timer<=0)enter(i,MantisPhase::ThrowCooldown);break;
        case MantisPhase::ThrowCooldown:
            if(k.timer<=0){
                if(k.throwsRemaining) --k.throwsRemaining;
                enter(i,k.throwsRemaining?MantisPhase::ThrowAntic:MantisPhase::WallLeavePause);
            }
            break;
        case MantisPhase::WallLeavePause:if(k.timer<=0)enter(i,MantisPhase::WallLeave1);break;
        case MantisPhase::WallLeave1:if(k.timer<=0)enter(i,MantisPhase::WallLeave2);break;
        case MantisPhase::WallLeave2:if(k.timer<=0)enter(i,MantisPhase::AfterThrowPause);break;
        case MantisPhase::AfterThrowPause:if(k.timer<=0)enter(i,MantisPhase::Idle);break;
        case MantisPhase::DeathStart:if(k.timer<=0)enter(i,MantisPhase::Leave);break;
        case MantisPhase::Leave:if(k.timer<=0)enter(i,MantisPhase::ReturnPause);break;
        case MantisPhase::ReturnPause:if(k.timer<=0){k.position=seats[i];k.scaleX=seatFacing[i];k.presentation.reset(k.position);enter(i,MantisPhase::Wounded);}break;
        default:break;
        }
        sync(i);
        if(objects_[i].colliderEnabled&&overlaps(bounds(i),bodyBounds(hero.position)))hero.takeDamage(1,hero.position.x<k.position.x?-1:1);
    }
    for(auto& p:lances)if(p.active){
        PresentationPosition::Step pose(p.presentation,p.position);p.age+=dt;
        // Shot Mantis Lord / Fly: mass1, no damping/gravity; Force opposes
        // initial velocity33 by24 units/s^2. Box2D integrates velocity first.
        if(p.age>=6.f){p.active=false;continue;}
        p.velocity.x-=p.direction*p.force*dt;
        p.position.x+=p.velocity.x*dt;p.position.y+=p.velocity.y*dt;
        if(overlaps(p.bounds(),bodyBounds(hero.position)))hero.takeDamage(1,hero.position.x<p.position.x?-1:1);
    }
}
void MantisLords::update(float dt){
    for(auto& k:lords)k.animation.advance(dt);
    for(auto& p:lances)if(p.active)p.animation.advance(dt);
}
}
