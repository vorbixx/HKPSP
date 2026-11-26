#pragma once
#include "monomon.hpp"
#include "animation.hpp"
#include "room.hpp"

namespace hk3ds {
struct MonomonRoomDraw {
    bool archive=false,guardian=false,bodyVisible=false,quirrelVisible=false,maskVisible=false;
    bool prompt=false,film=false,maskScene=false;
    float whiteFade=0;
    float glowAlpha=0;
    Vec2 glow{};
    AnimationPlayer glowAnimation;
    Vec2 body{},quirrel{},mask{};
    float bodyScale=1,quirrelScale=1;
    AnimationPlayer bodyAnimation,quirrelAnimation,maskAnimation;
    const char* text=nullptr;
};
// Archive-specific admission and reward owner. No renderer or save-file IO.
class MonomonRoom {
    friend struct SimulationStateHash;
public:
    bool enter(const char* root,const char* scene,const NailBank&,const SavedPlayerData&);
    void reset();
    void fixedUpdate(Hero&);
    bool update(float,Hero&,const Input&,SavedPlayerData&,bool dreamNailOwned=false);
    void capture(MonomonRoomDraw&)const;
    bool active()const{return archive_||guardian_||maskScene_;}
    bool guardian()const{return guardian_;}
    bool hasArt()const{return archive_||guardian_;}
    bool locked()const;
    float visualAlpha(unsigned owner)const;
    void present(RoomDraw&)const;
    bool cinematicRequested()const{return guardian_&&rewardCommitted_&&dreamer_.phase()==MonomonPhase::Cinematic;}
    bool cinematicEnded(){return cinematicRequested()&&dreamer_.cinematicEnded();}
    CombatScene& combat(){return combat_;}
    const RoomGate* transition()const{return transitionReady_?&transition_:nullptr;}
    MonomonDreamer& dreamer(){return dreamer_;}
    const AnimationBank& art()const{return art_;}
private:
    enum class Phase { Hidden,LeapPause,Leap,Landing,Idle,Talking,Summoning,Ready,Entering };
    void play(AnimationPlayer&,const char*);
    void nextDialogue();
    AnimationBank art_;
    AnimationPlayer body_,quirrel_,mask_,glow_;
    MonomonDreamer dreamer_;
    CombatObject object_{};
    CombatScene combat_;
    RoomGate transition_{};
    Phase phase_=Phase::Hidden;
    float timer_=0,orbAge_=-1,glowAge_=-1;
    unsigned conversation_=0,stage_=0;
    const char* text_=nullptr;
    bool archive_=false,guardian_=false,maskScene_=false,prompt_=false,verticalHeld_=false;
    bool transitionReady_=false,defeated_=false,rewardCommitted_=false;
};
}
