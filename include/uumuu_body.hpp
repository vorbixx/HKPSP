#pragma once
#include "animation.hpp"

namespace hk3ds {
enum class UumuuBodyAnimation { Idle, Attack, Wound, RecoverAntic, Recover };
struct UumuuBodyEvents {
    // Edge emitted once if the active source clip completes. The five shipped
    // body clips loop, so this remains false for the validated source bank.
    bool animationFinished=false;
    UumuuBodyAnimation finishedAnimation=UumuuBodyAnimation::Idle;
};

// Presents Mega Jellyfish's five body clips at explicit source FSM entry
// callbacks. The bank is borrowed and must outlive this presenter.
class UumuuBodyActor {
    friend struct SimulationStateHash;
public:
    bool bind(const AnimationBank&);
    void reset();
    // Every entry restarts the selected clip, including repeated Recover
    // entries. Source actions map Attack Antic -> Attack; Slash -> Wound;
    // Attack Recover and Recover -> Recover; Recover Antic -> RecoverAntic.
    // The actor starts on Idle. The room forwards those FSM entry callbacks.
    bool enter(UumuuBodyAnimation);
    bool advance(float dt);
    UumuuBodyEvents takeEvents(){const auto e=events_;events_={};return e;}
    UumuuBodyAnimation animation()const{return animation_;}
    const SpriteFrame* current()const{return bank_?player_.current(*bank_):nullptr;}
    const AnimationPlayer& player()const{return player_;}
    bool bound()const{return bank_!=nullptr;}
private:
    const AnimationBank* bank_=nullptr;
    const Clip* clips_[5]{};
    AnimationPlayer player_{};
    UumuuBodyAnimation animation_=UumuuBodyAnimation::Idle;
    UumuuBodyEvents events_{};
};
}
