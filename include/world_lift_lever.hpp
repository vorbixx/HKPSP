#pragma once
#include "world_switches.hpp"
#include <array>

namespace hk3ds {
// Town GO316/FSM3919. Values and identity are supplied by the room export.
struct MineLiftLeverRules { float hitWait=0; };
enum class MineLiftLeverActionKind { Hit, ShakeCamera, Open, DisableCollider, RestorePose, Activate };
struct MineLiftLeverAction { MineLiftLeverActionKind kind{}; };
struct MineLiftLeverEvents {
    std::array<MineLiftLeverAction,4> actions{}; unsigned count=0;
    void add(MineLiftLeverActionKind k){if(count<actions.size())actions[count++]={k};}
};
class MineLiftLever {
    friend struct SimulationStateHash;
public:
    enum class Phase { Unbound, Idle, Hit, Complete, Restored };
    bool bind(MineLiftLeverRules rules,SceneBoolHooks save,const char* scene,const char* id){
        if(!std::isfinite(rules.hitWait)||rules.hitWait<0||!save.read||!save.write||
           !scene||!*scene||!id||!*id)return false;
        MineLiftLever next;next.rules_=rules;next.save_=save;next.scene_=scene;next.id_=id;
        next.phase_=save.read(save.context,scene,id)?Phase::Restored:Phase::Idle;
        if(next.phase_==Phase::Restored){next.pending_.add(MineLiftLeverActionKind::RestorePose);
            next.pending_.add(MineLiftLeverActionKind::DisableCollider);
            next.pending_.add(MineLiftLeverActionKind::Activate);}
        *this=next;return true;
    }
    MineLiftLeverEvents hit(bool inRange,int attackType){
        MineLiftLeverEvents e;
        if(phase_!=Phase::Idle||!inRange||attackType!=0||!save_.write(save_.context,scene_,id_,true))return e;
        phase_=Phase::Hit;remaining_=rules_.hitWait;e.add(MineLiftLeverActionKind::Hit);
        e.add(MineLiftLeverActionKind::ShakeCamera);return e;
    }
    MineLiftLeverEvents update(float dt){
        MineLiftLeverEvents e;
        if(phase_!=Phase::Hit||!std::isfinite(dt)||dt<0)return e;
        remaining_-=dt;
        if(remaining_<=0){phase_=Phase::Complete;e.add(MineLiftLeverActionKind::Open);
            e.add(MineLiftLeverActionKind::DisableCollider);}
        return e;
    }
    MineLiftLeverEvents takeEvents(){auto e=pending_;pending_={};return e;}
    Phase phase()const{return phase_;}
private:
    MineLiftLeverRules rules_{}; SceneBoolHooks save_{};
    const char* scene_=nullptr;const char* id_=nullptr;
    MineLiftLeverEvents pending_{};float remaining_=0;Phase phase_=Phase::Unbound;
};
static_assert(sizeof(MineLiftLever)<=128,"Town lever state budget");
}
