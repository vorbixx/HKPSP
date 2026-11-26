#pragma once
#include "uumuu.hpp"
#include "uumuu_motion.hpp"

namespace hk3ds {
enum class UumuuAttackPhase { Inactive,Idle,Antic,Zapping,FirstZapPause,Multizap,Recover,Quirrel };
struct UumuuAttackEvents {
    bool antic=false,recover=false;
    // Includes repeated entries into Zapping, whose public phase is unchanged.
    bool enterMotion=false;
    UumuuMotionMode motion=UumuuMotionMode::Coast;
    // Spawn at this pose. The source FSM also requests Control/Antic Time,
    // but the exported MegaJellyZap prefab has no such FSM; the script's own
    // coroutine waits remain authoritative. These requests do not inflict damage.
    bool spawnZap=false;
    Vec2 zapPosition{};
    float zapAnticipation=0;
    // Select a random child of the original Multizaps object and activate its
    // children. Its pattern geometry is deliberately left to the scene adapter.
    bool activateRandomPattern=false,callQuirrel=false;
};

// Mega Jellyfish's attack cycle, entered by Start/FINISHED. Wake-up, Quirrel's
// actor/animation and actual lightning pool objects belong to room adapters.
class UumuuAttacks {
    friend struct SimulationStateHash;
public:
    void reset();
    bool start(EnemyRandom);
    void fixedUpdate(float dt,Vec2 heroPosition,UumuuQuirrel&);
    // Called only after the authored Recover -> Quirrel Roam state sequence.
    bool afterQuirrelRecovery(UumuuQuirrel&);
    UumuuAttackPhase phase()const{return phase_;}
    // Drain after each fixedUpdate, before advancing another source frame.
    UumuuAttackEvents takeEvents(){const auto result=events_;events_={};return result;}
private:
    void idle();
    void choose();
    void checkQuirrel(UumuuQuirrel&);
    UumuuAttackPhase phase_=UumuuAttackPhase::Inactive;
    EnemyRandom random_{};
    float timer_=0;
    unsigned zapCount_=0,chaseRepeats_=0,multizapRepeats_=0;
    UumuuAttackEvents events_{};
};
}
