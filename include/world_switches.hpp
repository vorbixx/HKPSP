#pragma once
#include <cmath>
#include <utility>

namespace hk3ds {
// write marks in-memory scene state; the save owner schedules serialization.
// Storage and disk policy belong to the save owner. Identity is the original
// PersistentBoolItem scene/object pair; no save bits or file format live here.
struct SceneBoolHooks {
    void* context=nullptr;
    bool (*read)(void*,const char* scene,const char* id)=nullptr;
    bool (*write)(void*,const char* scene,const char* id,bool value)=nullptr;
};
struct TollSwitchRules {
    float hitWait=0;
    unsigned freezeMoment=0,activatedFrame=0;
};
struct TollSwitchEvents {
    bool switchHit=false,restorePose=false;
    unsigned freezeMoment=0,activatedFrame=0;
    unsigned openGates=0,removeGates=0,detachChildren=0;
};
// The researched two-gate switch subset, not a general PlayMaker interpreter.
// The adapter supplies nail overlap, scaled frame time and animation FINISHED.
// Gate collision stays live until the corresponding removal event is applied.
class TollSwitch {
    friend struct SimulationStateHash;
public:
    enum class Phase {Unbound,Idle,Hit,Opening,Complete,Restored};
    static constexpr unsigned gateCount=2,allGates=(1u<<gateCount)-1;
    // Borrowed identity strings must outlive this binding.
    bool bind(TollSwitchRules rules,SceneBoolHooks save,const char* scene,const char* id)
    {
        if(!save.read||!save.write||!scene||!*scene||!id||!*id||
           !std::isfinite(rules.hitWait)||rules.hitWait<0)return false;
        TollSwitch next;
        next.rules_=rules;next.save_=save;next.scene_=scene;next.id_=id;
        next.activated_=save.read(save.context,scene,id);
        next.phase_=next.activated_?Phase::Restored:Phase::Idle;
        if(next.activated_){
            next.removed_=allGates;
            next.pending_.restorePose=true;next.pending_.activatedFrame=rules.activatedFrame;
            next.pending_.removeGates=allGates; // Activate destroys without detaching.
        }
        *this=next;return true;
    }
    bool reload(){return bind(rules_,save_,scene_,id_);}
    bool nailHit()
    {
        if(phase_!=Phase::Idle)return false;
        // A full or invalid save table must not open a gate that closes on load.
        if(!save_.write(save_.context,scene_,id_,true))return false;
        activated_=true;phase_=Phase::Hit;remaining_=rules_.hitWait;
        pending_.switchHit=true;pending_.freezeMoment=rules_.freezeMoment;
        return true;
    }
    // Call on subsequent scaled frames, as for PlayMaker Wait(realTime=false).
    void advance(float scaledSeconds)
    {
        if(phase_!=Phase::Hit||!std::isfinite(scaledSeconds)||scaledSeconds<0)return;
        if(scaledSeconds<remaining_){remaining_-=scaledSeconds;return;}
        remaining_=0;phase_=Phase::Opening;pending_.openGates|=allGates;
    }
    bool gateAnimationFinished(unsigned gate)
    {
        if(phase_!=Phase::Opening||gate>=gateCount||(removed_&(1u<<gate)))return false;
        const auto bit=1u<<gate;removed_|=bit;
        pending_.removeGates|=bit;pending_.detachChildren|=bit;
        if(removed_==allGates)phase_=Phase::Complete;
        return true;
    }
    TollSwitchEvents takeEvents(){return std::exchange(pending_,{});}
    Phase phase()const{return phase_;}
    bool activated()const{return activated_;}
    unsigned removedGates()const{return removed_;}
private:
    TollSwitchRules rules_{};
    SceneBoolHooks save_{};
    const char* scene_=nullptr;
    const char* id_=nullptr;
    TollSwitchEvents pending_{};
    float remaining_=0;
    unsigned removed_=0;
    Phase phase_=Phase::Unbound;
    bool activated_=false;
};
static_assert(sizeof(TollSwitch)<=128,"Keep world switch state bounded on host and ARM");
}
