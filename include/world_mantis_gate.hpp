#pragma once
#include "world_switches.hpp"

namespace hk3ds {
struct MantisGateRules {
    float hitWait=0,openPause=0,dustWait=0;
};
struct MantisGateEvents {
    bool switchHit=false,activatedPose=false,openGate=false,startDust=false,stopDust=false;
    bool firstClip=false,secondClip=false,removeCollider=false,hideGate=false;
    unsigned freezeMoment=0;
};
// Fungus2_04 Switch Control5074 -> Mantis Gate5078. The room adapter supplies
// authored nail-polygon overlap, scaled time and each clip's FINISHED event.
// Identity strings and the save owner's context must outlive the binding.
class MantisGate {
    friend struct SimulationStateHash;
public:
    enum class Phase {Unbound,Idle,Hit,OpenPause,Dust,Opening,Opening2,Opened};
    bool bind(MantisGateRules rules,SceneBoolHooks save,const char* scene,const char* id){
        if(!save.read||!save.write||!scene||!*scene||!id||!*id||
           !std::isfinite(rules.hitWait)||rules.hitWait<=0||
           !std::isfinite(rules.openPause)||rules.openPause<=0||
           !std::isfinite(rules.dustWait)||rules.dustWait<=0)return false;
        MantisGate next;next.rules_=rules;next.save_=save;next.scene_=scene;next.id_=id;
        next.phase_=Phase::Idle;
        if(save.read(save.context,scene,id)){
            next.phase_=Phase::Opened;
            // Saved Activated sends ACTIVATE: hide and remove collision without
            // playing the live OPEN waits, clips, dust or sound.
            next.pending_.activatedPose=next.pending_.removeCollider=next.pending_.hideGate=true;
        }
        *this=next;return true;
    }
    void clear(){*this={};}
    bool reload(){return phase_==Phase::Unbound||bind(rules_,save_,scene_,id_);}
    bool nailHit(){
        if(phase_!=Phase::Idle||!save_.write(save_.context,scene_,id_,true))return false;
        // Activated is set in Hit before its Wait. Refuse a failed save before
        // changing any runtime state or sending freeze/presentation events.
        phase_=Phase::Hit;elapsed_=0;pending_.switchHit=true;pending_.freezeMoment=1;return true;
    }
    void advance(float scaledSeconds,bool animationFinished=false){
        if(!std::isfinite(scaledSeconds)||scaledSeconds<=0)return;
        if(phase_==Phase::Hit||phase_==Phase::OpenPause||phase_==Phase::Dust)elapsed_+=scaledSeconds;
        switch(phase_){
        case Phase::Hit:
            if(elapsed_>=rules_.hitWait){phase_=Phase::OpenPause;elapsed_=0;pending_.openGate=true;}
            break;
        case Phase::OpenPause:
            if(elapsed_>=rules_.openPause){phase_=Phase::Dust;elapsed_=0;pending_.startDust=true;}
            break;
        case Phase::Dust:
            if(elapsed_>=rules_.dustWait){phase_=Phase::Opening;elapsed_=0;pending_.stopDust=pending_.firstClip=true;}
            break;
        case Phase::Opening:
            if(animationFinished){phase_=Phase::Opening2;pending_.secondClip=pending_.removeCollider=true;}
            break;
        case Phase::Opening2:
            if(animationFinished){phase_=Phase::Opened;pending_.hideGate=true;}
            break;
        default:break;
        }
    }
    Phase phase()const{return phase_;}
    bool bound()const{return phase_!=Phase::Unbound;}
    MantisGateEvents takeEvents(){const auto result=pending_;pending_={};return result;}
private:
    MantisGateRules rules_{};SceneBoolHooks save_{};const char* scene_=nullptr;const char* id_=nullptr;
    Phase phase_=Phase::Unbound;float elapsed_=0;MantisGateEvents pending_{};
};
}
