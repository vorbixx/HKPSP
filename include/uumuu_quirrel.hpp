#pragma once
#include "animation.hpp"
#include "uumuu_encounter.hpp"

namespace hk3ds {
// Named original FSM audio actions. The room resolves original clip/pool refs;
// the weighted voice action also owns its RNG draw, before pose sampling.
enum class UumuuQuirrelCue { CheckPosition,Arrive,Leave,SlashVoice,SlashAntic,Slash,Wound,
    WatchVoice,WatchIntro,WatchArrive,WatchLeave };
struct UumuuQuirrelAdapter {
    void* context=nullptr;
    void (*audio)(void*,UumuuQuirrelCue,Vec2)=nullptr;
    float (*floatRange)(void*,float,float)=nullptr;
    int (*intRange)(void*,int,int)=nullptr;
};
struct UumuuQuirrelSprite {
    float artScale() const {return animationFacingScale("uumuu-quirrel/Quirrel",-scaleX,std::abs(scaleX));}
    Vec2 localPosition{};
    float localZ=0,scaleX=1,rotationZ=0;
    bool visible=false;
    AnimationPlayer animation{};
};
// Presentation of the two real children of Mega Jellyfish transform47. Room/source
// banks and the encounter outlive this actor; reset before reusing either.
class UumuuQuirrelActor {
    friend struct SimulationStateHash;
public:
    bool bind(UumuuEncounter&,const AnimationBank& land,const AnimationBank& slash,
              UumuuQuirrelAdapter);
    void reset();
    // Drain immediately after every coordinator callback. This starts clips,
    // but never advances newly entered animations using already elapsed time.
    bool consume();
    bool watch(); // Original local WATCH event; only Idle accepts it.
    // Scaled Watch FSM Update, separate from animation and encounter timers.
    bool controlUpdate(float dt);
    // Separate animation callback: only a real clip completion crosses the
    // departure/Slash Antic/Slash boundaries. Does not tick encounter timers.
    bool animationUpdate(float dt,Vec2 heroPosition);
    const UumuuQuirrelSprite& land()const{return land_;}
    const UumuuQuirrelSprite& slash()const{return slash_;}
    // Inherit the parent's one shared presentation phase, with local offsets.
    Vec2 renderLandPosition(PresentationTime)const;
    Vec2 renderSlashPosition(PresentationTime)const;
    bool faulted()const{return faulted_;}
private:
    enum class Phase { Hidden,Arrive,Leave,Antic,Slash,Finished };
    enum class WatchPhase { Idle,Pause,NoChoice,Watching,Leap };
    void audio(UumuuQuirrelCue,Vec2);
    Vec2 position(const UumuuQuirrelSprite&)const;
    bool watchPause();
    bool watchPosition();
    UumuuEncounter* encounter_=nullptr;
    UumuuQuirrelAdapter adapter_{};
    const Clip* arrive_=nullptr;
    const Clip* leave_=nullptr;
    const Clip* antic_=nullptr;
    const Clip* slashClip_=nullptr;
    const Clip* watchClip_=nullptr;
    UumuuQuirrelSprite land_{},slash_{};
    Phase phase_=Phase::Hidden;
    WatchPhase watchPhase_=WatchPhase::Idle;
    float watchTimer_=0;
    unsigned previousWatchPosition_=0;
    bool roam_=false,voiceIntro_=false;
    bool faulted_=false;
};
}
