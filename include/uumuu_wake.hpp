#pragma once
#include "physics.hpp"

namespace hk3ds {
enum class UumuuWakePhase { Sleep,Pause,Rumble,Burst,Start,Ready };
struct UumuuWakeEvents {
    // Drain after each callback. Rumble: play audio, RumblingMed on, Spore/
    // Steam on. Burst: stop/play audio, begin tween, colliders on, rumble off,
    // BigShake, Steam/Spore off, Bub/Emerge Spore on, in that source order.
    bool rumble=false,burst=false;
    // Tween FINISHED enters Music then Start synchronously in the regular
    // archive. Issue snapshot/cue, then Start audio, body on/Entry Sprite off,
    // Emerge Spore off and title setup immediately; only Idle waits one second.
    bool music=false,start=false,ready=false;
};
// Mega Jellyfish/Control's regular-archive wake sequence. Scaled Update waits
// and iTween updates are separate callbacks; neither invents animation events.
class UumuuWake {
    friend struct SimulationStateHash;
public:
    void reset(){*this=UumuuWake{};}
    bool battleStart();
    void controlUpdate(float scaledDt);
    // Apply eased relative deltas to the current transform, preserving other
    // translations just as iTween.ApplyMoveByTargets does. False is no change.
    bool tweenUpdate(float scaledDt,Vec2& transform);
    UumuuWakePhase phase()const{return phase_;}
    bool collidersEnabled()const{return phase_>=UumuuWakePhase::Burst;}
    bool bodyVisible()const{return phase_>=UumuuWakePhase::Start;}
    UumuuWakeEvents takeEvents(){auto e=events_;events_={};return e;}
private:
    UumuuWakePhase phase_=UumuuWakePhase::Sleep;
    float timer_=0,tweenElapsed_=0,tweenOffset_=0;
    UumuuWakeEvents events_{};
};
}
