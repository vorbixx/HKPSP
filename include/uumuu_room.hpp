#pragma once
#include "uumuu_encounter.hpp"
#include "uumuu_quirrel.hpp"
#include "uumuu_audio.hpp"
#include "uumuu_blood.hpp"
#include <vector>

namespace hk3ds {
struct UumuuRoomDraw {
    float artScale() const {return animationFacingScale("uumuu-body/Uumuu",1);}
    bool active=false,visible=false,dead=false,locked=false;
    Vec2 position{};
    AnimationPlayer body{};
    UumuuQuirrelSprite land{},slash{};
    Vec2 landPosition{},slashPosition{};
    struct Zap {Vec2 position{};bool active=false;AnimationPlayer animation{};float scaleX=1,rotation=0;};
    std::array<Zap,UumuuEncounter::zapCapacity+UumuuMultizaps::capacity> zaps{};
    unsigned zapCount=0;
    std::array<UumuuBlood::Draw,UumuuBlood::capacity> blood{};
    unsigned bloodCount=0;
    struct Gate {bool visible=false;AnimationPlayer animation;};
    std::array<Gate,UumuuGates::capacity> gates{};
};
// Scene-local owner. Source controllers retain their independent callbacks.
class UumuuRoom {
    friend struct SimulationStateHash;
public:
    bool enter(const char* root,const char* scene,const NailBank&,EnemyRandom,UumuuProgress&);
    void reset();
    void update(float,Hero&,bool frozen=false);
    void fixedUpdate(float,Hero&,bool frozen=false);
    World world(const World&);
    void capture(UumuuRoomDraw&,PresentationTime)const;
    bool active()const{return active_;}
    bool takeTitleRequest(){const bool value=titleRequested_;titleRequested_=false;return value;}
    bool locked()const{return active_&&encounter_.locked();}
    bool faulted()const{return faulted_;}
    CombatScene& combat(){return combat_;}
    Vec2 position()const{return encounter_.motion().position();}
    bool completed()const{return encounter_.completed();}
    int hp()const{return object_.health.hp;}
    UumuuShellPhase shellPhase()const{return encounter_.shellPhase();}
    bool gatesOpen()const;
    const UumuuAudioEvents& audio()const{return audio_;}
private:
    void quirrelAudio(UumuuQuirrelCue,Vec2);
    CombatObject object_{}; // Must outlive the borrowed encounter binding.
    UumuuGates gates_;
    UumuuEncounter encounter_;
    UumuuBodyActor body_;
    UumuuQuirrelActor quirrel_;
    AnimationBank bodyArt_,quirrelArt_,gateArt_,effectsArt_;
    std::array<AnimationPlayer,UumuuMultizaps::capacity> lightningAnimations_{};
    std::array<AnimationPlayer,UumuuGates::capacity> gateAnimations_{};
    CombatScene combat_;
    std::vector<Solid> solids_;
    World recoilWorld_{};
    Vec2 recoilBody_{},recoilTransform_{};
    EnemyRandom random_{};
    UumuuAudioEvents audio_{};
    UumuuBlood blood_;
    float bloodClock_=0;
    std::array<float,UumuuGates::capacity> gateOpenTime_{};
    bool active_=false,faulted_=false,corpse_=false,titleRequested_=false;
};
}
