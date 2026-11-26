#pragma once
#include "combat.hpp"
#include "enemy.hpp"

namespace hk3ds {
// Damage-gate component, not Uumuu's movement/lightning or Quirrel's actor FSM.
// The owning animation adapter must deliver the authored Slash/FINISHED events.
enum class UumuuShellPhase { Guarded, Slash, Wounded, RecoverAntic, Recover, Dead };
class UumuuShell {
    friend struct SimulationStateHash;
public:
    UumuuShell()=default;
    ~UumuuShell(){reset();}
    UumuuShell(const UumuuShell&)=delete;
    UumuuShell& operator=(const UumuuShell&)=delete;
    // Borrowed slot outlives this component. Reset restores health and the
    // borrowed BounceShroom presence; collider/movement ownership stays outside.
    bool bind(CombatObject&);
    void reset();
    bool beginSlash();
    // Object.Destroy is deferred. The native room adapter completes this at
    // the proven component-destruction boundary, never at an inferred timer.
    std::uint32_t takeBounceShroomDestroyRequest(){
        const auto token=destroyRequest_?pendingDestroy_:0;destroyRequest_=false;return token;
    }
    bool bounceShroomDestroyed(std::uint32_t token);
    bool slashAnimationFinished();
    void fixedUpdate(float dt);
    UumuuShellPhase phase()const{return phase_;}
    bool takeLethalHit(){const bool value=lethal_;lethal_=false;return value;}
private:
    CombatObject* object_=nullptr;
    EnemyHealth previousHealth_{};
    bool previousHasHealth_=false,previousBounceShroom_=false;
    std::uint32_t destroySerial_=0,pendingDestroy_=0;
    bool destroyRequest_=false;
    UumuuShellPhase phase_=UumuuShellPhase::Guarded;
    float timer_=0;
    bool lethal_=false;
    static void receive(void*,const NailHit&,HitOutcome);
};

// Typed outputs of Battle Scene/Control; the room adapter consumes broadcasts
// and child activation, while the save owner consumes encounteredMegaJelly.
struct UumuuBattleEvents {
    bool encounteredMegaJelly=false,battleStart=false;
    bool closeGates=false,openGates=false,quickOpenGates=false;
    bool activateChildren=false,deactivateChildren=false;
};

struct UumuuDeathEvents;
// Battle Scene/Control consumes the corpse's BOSS DEATH separately from HP loss.
class UumuuBattle {
    friend struct SimulationStateHash;
public:
    void bind(bool defeatedMegaJelly);
    bool arenaEntered();
    bool bossDeath();
    bool onDeathEvents(const UumuuDeathEvents&);
    void fixedUpdate(float dt);
    bool locked()const{return stage_==Stage::Active||stage_==Stage::EndPause;}
    bool encountered()const{return encountered_;}
    bool completed()const{return stage_==Stage::Complete;}
    UumuuBattleEvents takeEvents(){const auto result=events_;events_={};return result;}
private:
    enum class Stage {Idle,Active,EndPause,Complete};
    Stage stage_=Stage::Idle;
    float timer_=0;
    bool encountered_=false;
    UumuuBattleEvents events_{};
};

enum class UumuuQuirrelDecision { Waiting,Idle,Call };
struct UumuuQuirrelEvents {
    bool watch=false,roamChanged=false,roam=false;
};
// The owning actor calls these at the named source state boundaries. Movement,
// arrival/animation completion and the Quirrel Land actor remain adapter-owned.
class UumuuQuirrel {
    friend struct SimulationStateHash;
public:
    UumuuQuirrel(){reset();}
    void reset();
    void idleUpdate(float dt);
    UumuuQuirrelDecision check(); // Mega Jellyfish/Quirrel?
    void setTimer(EnemyRandom);
    bool arrivePause(); // true selects ATTACK; false selects the first arrival.
    void roam();
    UumuuQuirrelEvents takeEvents(){const auto result=events_;events_={};return result;}
    float elapsed()const{return timer_;}
    float deadline()const{return deadline_;}
private:
    float timer_=0,deadline_=0;
    bool arrived_=false;
    UumuuQuirrelEvents events_{};
};
}
