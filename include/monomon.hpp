#pragma once
#include "combat.hpp"
#include "monomon_data.hpp"
#include "saved_player_data.hpp"

namespace hk3ds {
// Atomic and idempotent: a full save table cannot leave half a Dreamer reward.
bool awardMonomon(SavedPlayerData&);
// Scene-local rules for level402/Dreamer Monomon/Control. The room owner
// supplies accepted contacts, focus eligibility and the actual film callback.
enum class MonomonPhase { Inactive,Guardian,Wounded,SuckPause,Sucking,Flashing,
                          EndReady,Ending,Cinematic,DoorPause,Complete };
class MonomonDreamer {
    friend struct SimulationStateHash;
public:
    void reset(bool alreadyDefeated=false);
    bool hit(const NailHit&);
    // A true result is the one-shot PlayerData commit after End Start.
    bool update(float seconds,bool focusHeld,bool canFocus);
    bool cinematicEnded();
    MonomonPhase phase() const {return phase_;}
    unsigned hitsRemaining() const {return hits_;}
    float focusElapsed() const {return focusElapsed_;}
    float animationSeconds() const {return animationSeconds_;}
    const char* clip() const;
    bool locked() const;
private:
    MonomonPhase phase_=MonomonPhase::Inactive;
    unsigned hits_=0;
    float timer_=0,hitPause_=0,focusElapsed_=0,animationSeconds_=0;
    bool hitAnimation_=false;
};
}
