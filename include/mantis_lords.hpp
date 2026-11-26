#pragma once
#include "enemy.hpp"

namespace hk3ds {
enum class MantisPhase { Seated, ThroneStand, ThroneLeave, StartPause, Arrive, Idle, DashAntic, Dash, DashEnd,
    StabAntic, Stab, Land, Wall, Throw, Leave, Wounded, Bow, DashArrive, DashLeave, StabLeave,
    ThrowAntic, ThrowCooldown, WallLeavePause, WallLeave1, WallLeave2, AfterThrowPause, Scheduled,
    DeathStart, ReturnPause };
struct MantisChoices {
    std::array<unsigned char,7> repeats{},missed{};
    unsigned select(EnemyRandom,unsigned count,const unsigned* repeatMax,const unsigned* missedMax);
};
struct MantisLord {
    float artScale()const{return animationFacingScale("mantis-lords/Lord",scaleX,std::abs(scaleX));}
    Vec2 position{},velocity{};
    float scaleX=1,timer=0;
    unsigned attack=0,throwsRemaining=0,pendingMove=0,throwMode=0;
    bool throwHigh=false;
    MantisChoices choices;
    bool fighting=false,defeated=false,throneRaised=false,lookExitPending=false;
    MantisPhase phase=MantisPhase::Seated;
    AnimationPlayer animation;
    PresentationPosition presentation;
    // Away immediately enters the hidden .4s Idle between attacks.
    bool visible()const{return phase!=MantisPhase::Idle&&phase!=MantisPhase::StartPause&&phase!=MantisPhase::AfterThrowPause&&phase!=MantisPhase::Scheduled&&phase!=MantisPhase::ReturnPause;}
    // level180 throne Transforms 76/77/99; combat stays on the hero plane.
    float renderDepth()const{
        return phase==MantisPhase::Seated||phase==MantisPhase::ThroneStand||phase==MantisPhase::ThroneLeave||phase==MantisPhase::Wounded||phase==MantisPhase::Bow?3.54f:.004f;
    }
};
struct MantisLance {
    Vec2 position{},origin{},velocity{};
    float direction=1,age=0,force=24;
    bool active=false,high=false;
    AnimationPlayer animation;
    PresentationPosition presentation;
    Rect bounds()const{
        // level180 BoxCollider2D189, including Transform139's 1.5 scale.
        return boxBounds(position,{-2.397249f,-.642683f,2.433847f,.697798f});
    }
};
// Fixed actors and projectiles; the resident room owns art and combat storage.
class MantisLords {
    friend struct SimulationStateHash;
public:
    static constexpr unsigned capacity=3,lanceCapacity=4;
    bool bind(CombatObject*,unsigned,const AnimationBank*,EnemyRandom,bool defeated=false);
    void reset(){*this=MantisLords{};}
    void fixedUpdate(float,Hero&);
    void update(float);
    bool active()const{return objects_!=nullptr;}
    bool locked()const{return stage_!=Stage::Rest&&stage_!=Stage::Complete;}
    bool completed()const{return stage_==Stage::Complete;}
    bool victoryEarned()const{return stage_==Stage::Victory||completed();}
    bool sceneryVisible()const{return active()&&((locked()&&introAge_>=cageTime)||(outroAge_>=0&&outroAge_<3.5f));}
    bool floorRetracted(unsigned side=0)const{return active()&&((locked()&&introAge_>=floorTime)||
        (stage_==Stage::Complete&&outroAge_>=0&&floorExtendAge_[side]<0));}
    float floorAge()const{return introAge_-floorTime;}
    const char* floorClip(unsigned side=0)const;
    float floorAnimationAge(unsigned side=0)const{return locked()?floorAge():std::max(0.f,floorExtendAge_[side]);}
    const char* heroClip()const;
    AnimationPlayer heroAnimation()const;
    bool challengeAvailable(Vec2)const;
    bool challenge(Hero&);
    float titleAlpha()const;
    float gateOffset()const;
    static bool gateOwner(unsigned owner){
        const auto source=owner&0x00ffffffu;
        return (owner&0xff000000u)==0x81000000u&&source!=7&&source!=8&&source!=19&&source!=20;
    }
    unsigned remaining()const;
    Rect bounds(unsigned)const;
    static Rect arena(){return {18.2f,6.08f,41.77f,22};}
    // level180 CameraLockArea369/370: centre fixed, side shafts can track upwards.
    static Rect cameraLimits(Vec2 hero){
        const auto body=bodyBounds(hero);
        const bool side=boxSensorOverlap(body,{39.0460758f,1.4780941f,45.2182522f,26.162756f})||
                        boxSensorOverlap(body,{14.4439125f,1.4780941f,20.6160889f,26.162756f});
        return {30.08f,11.5f,30.08f,side?18.31f:11.5f};
    }
    std::array<MantisLord,capacity> lords{};
    std::array<MantisLance,lanceCapacity> lances{};
private:
    enum class Stage {Rest,Intro,Solo,Intermission,Pair,Victory,Complete};
    // Hero Challenge Start: 12 frames / 12fps, then delayed event .7s.
    // Throne Stand waits 1s; Cage waits 2s; Floor waits 1s.
    static constexpr float standTime=1.7f,cageTime=2.7f,floorTime=4.7f,endTime=5.7f;
    static constexpr float regainTime=endTime+2.f/12; // Hero Challenge End.
    struct Receiver {MantisLords* owner=nullptr;unsigned index=0;};
    std::array<Receiver,capacity> receivers_{};
    CombatObject* objects_=nullptr;
    const AnimationBank* art_=nullptr;
    EnemyRandom random_{};
    Stage stage_=Stage::Rest;
    float timer_=0,introAge_=0,outroAge_=-1;
    std::array<float,2> floorExtendAge_{{-1,-1}};
    bool upHeld_=false,heroFacing_=true;
    void enter(unsigned,MantisPhase);
    void choose(unsigned,const Hero&);
    void sync(unsigned);
    void launch(unsigned);
    float play(AnimationPlayer&,const char*);
    void startMove(unsigned,const Hero&,unsigned);
    void schedule(unsigned,unsigned,float,const Hero&);
    void choosePair(const Hero&);
    static void receive(void*,const NailHit&,HitOutcome);
    MantisChoices pairChoices_;
    float pairWait_=0;
    bool secondDash_=false,secondDashLeft_=false;
};
}
