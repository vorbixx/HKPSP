#pragma once
#include "door_prompt_presentation.hpp"
#include <cstdio>
#include <cstring>

namespace hk3ds {
// Top-screen Arrow Prompt for gameplay actions near an NPC, bench or machine (Listen, Rest,
// Shop, Inspect). Its existing presentation uses the arrow art, label font and
// .4 s up / .233 s down fade; the newer door prefab has a separate object lifecycle. Prompts for
// the bottom screen are never drawn here. Presentation only.
struct ArrowPromptRequest {
    const char* label=nullptr;   // null or empty: no prompt
    Vec2 position{};
    bool authoredAnchor=false;
};
class ArrowPrompt {
    friend struct SimulationStateHash;
public:
    const ArrowPromptPresentation& presentation() const {return state_;}
    void reset(){state_.reset();gate_={};}
    void update(float seconds,const ArrowPromptRequest& request){
        const bool show=request.label&&*request.label;
        if(show){
            std::snprintf(gate_.prompt,sizeof(gate_.prompt),"%s",request.label);
            gate_.promptPosition=request.position;
        }
        state_.update(seconds,show?&gate_:nullptr);
        // A changed label or anchor must show at once on the already visible prompt.
        if(show){state_.authoredAnchor=request.authoredAnchor;state_.gate.promptPosition=gate_.promptPosition;std::memcpy(state_.gate.prompt,gate_.prompt,sizeof(gate_.prompt));}
    }
private:
    ArrowPromptPresentation state_;
    RoomGate gate_{};
};
}
