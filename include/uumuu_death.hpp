#pragma once

namespace hk3ds {
enum class UumuuDeathPhase {Inactive,Init,Steam,Ready,Blown};
struct UumuuDeathEvents {
    bool defeatedMegaJelly=false,musicSnapshot=false;
    bool init=false,steam=false,ready=false,blow=false,bossDeath=false;
};

// Corpse Mega Jellyfish/Death gameplay sequencing. Activate when the original
// corpse is spawned, not directly from HP loss. Effects/audio remain adapters;
// the save owner consumes defeatedMegaJelly at activation, before BOSS DEATH.
class UumuuDeath {
    friend struct SimulationStateHash;
public:
    bool activate(bool bossScene);
    void fixedUpdate(float scaledDt);
    void reset(){*this=UumuuDeath{};}
    UumuuDeathPhase phase()const{return phase_;}
    UumuuDeathEvents takeEvents(){const auto result=events_;events_={};return result;}
private:
    UumuuDeathPhase phase_=UumuuDeathPhase::Inactive;
    float timer_=0;
    UumuuDeathEvents events_{};
};
}
