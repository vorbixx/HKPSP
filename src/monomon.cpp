#include "monomon.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
namespace hk3ds {
bool awardMonomon(SavedPlayerData& data) {
    if(data.get("monomonDefeated"))return true;
    SavedPlayerData next=data;
    const auto guardians=next.get("guardiansDefeated");
    if(guardians<0||guardians==std::numeric_limits<int>::max())return false;
    const auto put=[&](const char* name,int value){return next.set(name,value);};
    if(!put("monomonDefeated",1)||!put("hornetFountainEncounter",1)||
       !put("guardiansDefeated",guardians+1)||!put("maskToBreak",2))return false;
    if(guardians==0&&!put("marmOutside",1))return false;
    if(!next.get("crossroadsInfected")&&
       (!put("crossroadsInfected",1)||!put("visitedCrossroads",0)))return false;
    if(guardians>=2){
        const auto bretta=next.get("brettaState");
        if(bretta==std::numeric_limits<int>::max()||!put("brettaState",bretta+1)||!put("mrMushroomState",1))return false;
        for(const char* flag:{"corniferAtHome","iseldaConvo1","dungDefenderSleeping",
            "corn_crossroadsLeft","corn_greenpathLeft","corn_fogCanyonLeft","corn_fungalWastesLeft",
            "corn_cityLeft","corn_waterwaysLeft","corn_minesLeft","corn_cliffsLeft","corn_deepnestLeft",
            "corn_outskirtsLeft","corn_royalGardensLeft","corn_abyssLeft","metIselda"})
            if(!put(flag,1))return false;
    }
    data=next;return true;
}
void MonomonDreamer::reset(bool defeated) {
    *this={};
    if(!defeated){phase_=MonomonPhase::Guardian;hits_=monomon_data::hits;}
}
bool MonomonDreamer::hit(const NailHit& hit) {
    if(phase_!=MonomonPhase::Guardian||hitPause_>0||hit.damage<=0||hit.type==AttackType::Spell)return false;
    hitPause_=monomon_data::hitPause;
    // Source Hit is LoopSection: repeated Play calls preserve its clock.
    if(!hitAnimation_){hitAnimation_=true;animationSeconds_=0;}
    if(--hits_==0){phase_=MonomonPhase::Wounded;animationSeconds_=0;}
    return true;
}
const char* MonomonDreamer::clip() const {
    if(phase_==MonomonPhase::Inactive)return nullptr;
    if(phase_==MonomonPhase::Guardian)return hitAnimation_?"Hit":"Idle";
    return "Wound";
}
bool MonomonDreamer::locked() const {
    return phase_>=MonomonPhase::SuckPause&&phase_!=MonomonPhase::Complete;
}
bool MonomonDreamer::cinematicEnded() {
    if(phase_!=MonomonPhase::Cinematic)return false;
    phase_=MonomonPhase::DoorPause;timer_=0;return true;
}
bool MonomonDreamer::update(float dt,bool held,bool eligible) {
    if(!std::isfinite(dt)||dt<=0||dt>.25f||phase_==MonomonPhase::Inactive)return false;
    animationSeconds_+=dt;hitPause_=std::max(0.f,hitPause_-dt);
    if(phase_==MonomonPhase::Guardian)return false;
    if(phase_>=MonomonPhase::Wounded&&phase_<MonomonPhase::Ending){
        if(!held||!eligible){phase_=MonomonPhase::Wounded;timer_=0;return false;}
        if(phase_==MonomonPhase::Wounded){phase_=MonomonPhase::SuckPause;timer_=0;}
    }
    float left=dt;
    for(unsigned step=0;step<8&&left>0;++step){
        float duration=0;MonomonPhase next=phase_;
        switch(phase_){
        case MonomonPhase::SuckPause:duration=monomon_data::suckPause;next=MonomonPhase::Sucking;break;
        case MonomonPhase::Sucking:duration=monomon_data::sucking;next=MonomonPhase::Flashing;break;
        case MonomonPhase::Flashing:duration=monomon_data::flashing;next=MonomonPhase::EndReady;break;
        case MonomonPhase::EndReady:
            focusElapsed_+=left;left=0;
            // Source FloatCompare sends END only for greater-than, not equal.
            if(focusElapsed_>monomon_data::drain){phase_=MonomonPhase::Ending;timer_=0;}
            continue;
        case MonomonPhase::Ending:duration=monomon_data::ending;next=MonomonPhase::Cinematic;break;
        case MonomonPhase::DoorPause:duration=monomon_data::doorPause;next=MonomonPhase::Complete;break;
        default:return false;
        }
        const float part=std::min(left,std::max(0.f,duration-timer_));
        timer_+=part;left-=part;
        if(phase_==MonomonPhase::Flashing)focusElapsed_+=part;
        if(timer_<duration)break;
        phase_=next;timer_=0;
        if(phase_==MonomonPhase::Cinematic)return true;
    }
    return false;
}
}
