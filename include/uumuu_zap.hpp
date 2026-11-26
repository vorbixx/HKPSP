#pragma once
#include "uumuu_attacks.hpp"

namespace hk3ds {
enum class UumuuZapPhase {Inactive,Antic,Active,EndPause};
// The renderer maps these to the original particles, fader and two audio events.
struct UumuuZapEvents {bool antic=false,strike=false,end=false,recycle=false;};

// One pooled MegaJellyZap.Type.Zap. A pool adapter supplies an inactive slot;
// active slots cannot be overwritten. Visuals and the pool itself are separate.
class UumuuZap {
    friend struct SimulationStateHash;
public:
    bool activate(Vec2);
    bool onAttackEvents(const UumuuAttackEvents&);
    void fixedUpdate(float);
    void contactHero(Hero&,Rect worldHeroBox)const;
    void deactivate();
    UumuuZapPhase phase()const{return phase_;}
    Vec2 position()const{return position_;}
    UumuuZapEvents takeEvents(){const auto result=events_;events_={};return result;}
private:
    UumuuZapPhase phase_=UumuuZapPhase::Inactive;
    Vec2 position_{};
    float timer_=0;
    UumuuZapEvents events_{};
};
}
