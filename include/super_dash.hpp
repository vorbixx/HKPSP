#pragma once

namespace hk3ds {
enum class SuperDashPhase { None, Charging, Charged, Flying, Braking, WallRecovery };
struct SuperDashState {
    SuperDashPhase phase=SuperDashPhase::None;
    float elapsed=0,stationary=0;
    int wall=0,direction=1;
    unsigned groundProbeFrame=0;
    bool groundProbeHit=true;
    bool active() const {return phase!=SuperDashPhase::None;}
    bool flying() const {return phase==SuperDashPhase::Flying;}
};
// Steam22529139 Knight Superdash FSM (c1-23052), not normal Cloak dash tuning.
namespace super_dash {
constexpr float charge=.8f,speed=30.f,cancelable=.2f;
constexpr float braking=.54f,wallRecovery=.5f,deceleration=.9f,stalled=.05f;
// resources.assets Knight3916 children4466/7664, transforms10064/11483.
constexpr float supportTopY=-.08900000155f,supportBottomY=-1.169999957f;
constexpr float wallProbeLength=.75f,groundProbeLength=2.f;
// resources.assets GO5366, trigger13743 and damages_enemy FSM20881.
constexpr float damageOffsetX=-.026382446f,damageOffsetY=-.6942673f;
constexpr float damageWidth=1.498964f,damageHeight=1.1697865f;
constexpr int damage=10;
constexpr float recoilMultiplier=1.5f;
}
}
