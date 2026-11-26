#pragma once
#include "enemy.hpp"
#include "watcher_progress.hpp"

namespace hk3ds {
enum class WatcherPhase { Rest,BugsIn,CloudStop,BugsEnd,RoarStart,Roar,RoarEnd,
    Idle,Turn,Run,RollStart,ChargeAntic,Charge,ChargeEnd,ChargeRecover,
    JumpAntic,AnticAir,JumpAir,Bounce,BounceAir,Land,SlashAntic,Slash1,
    Slash1Recover,Slash2,Slash2Recover,Recover,BackRoll,BackRollEnd,Dead };
struct WatcherKnight {
    float artScale()const{return animationFacingScale("watcher-knights/Watcher",-scaleX,std::abs(scaleX));}
    Vec2 position{},velocity{};
    float scaleX=1,timer=0,secondTimer=0,charge=0,jumpX=0;
    float idleMin=0,idleMax=0,hitFlash=0;
    WatcherPhase phase=WatcherPhase::Rest;
    unsigned attack=0,attacks=0;
    std::array<unsigned,4> repeats{};
    bool contact=false,collider=false,doubleAttack=false;
    AnimationPlayer animation;
    PresentationPosition presentation;
    Vec2 renderPosition(PresentationTime t)const{return presentation.sample(position,t);}
    bool alive()const{return phase!=WatcherPhase::Dead;}
};
class WatcherEncounter {
    friend struct SimulationStateHash;
public:
    static constexpr unsigned capacity=6;
    bool bind(CombatObject*,unsigned,const AnimationBank*,EnemyRandom,
              WatcherProgress={},unsigned nailSmithUpgrades=0);
    void reset();
    void fixedUpdate(float,Hero&,const World&);
    void update(float);
    bool active()const{return objects_!=nullptr;}
    bool locked()const{return stage_!=Stage::Idle&&stage_!=Stage::Complete;}
    bool completed()const{return stage_==Stage::Complete;}
    unsigned remaining()const{return remaining_;}
    unsigned awake()const;
    WatcherProgress progress()const{return progress_;}
    Rect bounds(unsigned)const;
    Rect arena()const;
    static const Rect& gate(unsigned);
    static const Rect& trigger();
    // Source Journal state is emitted once, independently of the later BG OPEN.
    bool takeJournalReward(){const bool result=journal_;journal_=false;return result;}
    void* progressContext=nullptr;
    void (*onProgress)(void*,const WatcherProgress&)=nullptr;
    std::array<WatcherKnight,capacity> knights{};
private:
    enum class Stage {Idle,Close,Waiting,Delay,VictoryPause,MusicEnd,Complete};
    struct Receiver {WatcherEncounter* owner=nullptr;unsigned index=0;};
    std::array<Receiver,capacity> receivers_{};
    CombatObject* objects_=nullptr;
    const AnimationBank* art_=nullptr;
    EnemyRandom random_{};
    WatcherProgress progress_{};
    Stage stage_=Stage::Idle;
    float timer_=0;
    unsigned remaining_=capacity,current_=0;
    bool journal_=false;
    static void receive(void*,const NailHit&,HitOutcome);
    void enter(unsigned,WatcherPhase);
    void idle(unsigned,bool retime);
    void choice(unsigned,const Hero&,bool inRange);
    void rollStart(unsigned,unsigned,const Hero&);
    void wake(unsigned);
    void battle(float,Hero&);
    void sync(unsigned);
    bool inRange(unsigned,Rect,const Hero&)const;
};
}
