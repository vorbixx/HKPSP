#pragma once
#include "enemy.hpp"
#include "broken_vessel_progress.hpp"
namespace hk3ds {
enum class BrokenVesselPhase { Sleep,StartPause,Rumble,Spawning,Wake1,Wake2,Wake3,Rewake,RoarStart,Roar,RoarEnd,
    Idle,JumpAntic,Jump,Land,DashAntic1,DashAntic2,DashAntic3,Dash1,Dash2,Dash3,DashRecover,
    DownstabAntic,Downstab,DownstabLand,DownstabRecover,EvadeAntic,Evade,EvadeRecover,OverheadAntic,Overhead,OverheadRecover,
    ShakeJumpAntic,ShakeJump,ShakeLand,ShakeAntic,ShakeStart,Shaking,ShakeEnd,StunAir,Stun,StunRecover,
    CorpseAir,CorpseLand,CorpseSteam,CorpseReady,CorpseBlow,DoorRumble,Complete };
struct BrokenVesselHazard {
    float artScale()const{return animationFacingScale("broken-vessel/Balloon",facing,scale);}
    Vec2 position{},velocity{};
    PresentationPosition presentation;
    AnimationPlayer animation;
    float life=0,scale=1,speed=0,timer=0,rotation=0,facing=1,acceleration=75;
    unsigned state=0;
    bool active()const{return life>0;}
    Vec2 renderPosition(PresentationTime t)const{return presentation.sample(position,t);}
};
struct BrokenVesselScuttler : BrokenVesselHazard {
    float artScale()const{return animationFacingScale("broken-vessel/Scuttler",-facing,scale);}
    Vec2 target{};
};
class BrokenVesselEncounter {
    friend struct SimulationStateHash;
public:
    float artScale()const{return animationFacingScale("broken-vessel/BrokenVessel",scaleX,std::abs(scaleX));}
    bool bind(CombatObject*,const AnimationBank*,EnemyRandom,BrokenVesselProgress={},unsigned objectCapacity=1);
    void reset(){*this=BrokenVesselEncounter{};}
    void fixedUpdate(float,Hero&,const World&);
    void update(float);
    bool active()const{return object_!=nullptr;}
    bool locked()const{return active()&&started_&&!battleEnded_;}
    bool exitOpen()const{return progress_.killed&&phase==BrokenVesselPhase::Complete;}
    bool visible()const{return active()&&(!progress_.killed||corpseVisible_);}
    BrokenVesselProgress progress()const{return progress_;}
    int health()const{return object_?object_->health.hp:0;}
    Rect bounds()const;
    static const Rect& gate();
    Vec2 renderPosition(PresentationTime t)const{return presentation_.sample(position,t);}
    Vec2 position{},velocity{};
    float scaleX=1,hitFlash=0;
    BrokenVesselPhase phase=BrokenVesselPhase::Sleep;
    AnimationPlayer animation;
    std::array<BrokenVesselHazard,32> projectiles{};
    std::array<BrokenVesselHazard,3> balloons{};
    std::array<BrokenVesselScuttler,24> scuttlers{};
private:
    CombatObject* object_=nullptr;
    const AnimationBank* art_=nullptr;
    EnemyRandom random_{};
    BrokenVesselProgress progress_{};
    PresentationPosition presentation_;
    float timer_=0,jumpX_=0,balloonTimer_=4,shakeSideTimer_=0,shakeUpTimer_=0;
    float scuttlerTimer_=0;
    unsigned objectCapacity_=1,shakeIndex_=0;
    bool airDash_=false,willEvade_=false,willOverhead_=false;
    int shakeDirection_=1;
    unsigned hits_=0,combo_=0,stuns_=0,lastAttack_=3;
    float comboTimer_=0;
    std::array<unsigned,3> repeats_{},missed_{};
    bool started_=false,battleEnded_=false,contact_=false,downstab_=false,corpseVisible_=false;
    static void receive(void*,const NailHit&,HitOutcome);
    static void receiveBalloon(void*,const NailHit&,HitOutcome);
    void enter(BrokenVesselPhase);
    void choose(const Hero&);
    void sync();
    void spawnProjectile(Vec2,Vec2);
    void updateHazards(float,Hero&,const World&);
    void updateScuttlers(float,const World&);
};
}
