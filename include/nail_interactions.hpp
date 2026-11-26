#pragma once
#include "geo_visual.hpp"
#include "audio.hpp"

namespace hk3ds {
// Source TinkEffect dispatch and the shared Arrow Prompt. Presentation only.
class NailInteractions {
    friend struct SimulationStateHash;
public:
    struct Spike { unsigned owner=0,count=0;std::array<Rect,2> bounds{};float scale=1.5f; };
    struct Prompt { unsigned owner=0;Vec2 position{};float depth=0; };
    bool load(const char*,const AnimationBank&);
    void bind(const AnimationBank&,const std::array<Spike,2>&,const std::array<Prompt,4>&);
    void reset();
    void update(float,const Hero&,const NailBank&,unsigned promptOwner,DecorEffects* effects=nullptr);
    void dispatchAudio(AudioVoices&,std::array<unsigned,2>&,Vec2 listener={}) const;
    const std::array<GeoVisualDraw,2>& tinks() const { return draws_; }
    const GeoVisualDraw& prompt() const { return prompt_; }
    unsigned promptOwner() const {return promptOwner_;}
    float labelAlpha() const;
    unsigned serial(unsigned i) const {return states_[i].serial;}
    const std::array<Spike,2>& spikes() const {return spikes_;}
    const std::array<Prompt,4>& prompts() const {return prompts_;}
private:
    struct State {float cooldown=0,pitch=1;unsigned attack=0,serial=0;bool overlap=false;};
    const AnimationBank* bank_=nullptr;
    std::array<Spike,2> spikes_{};
    std::array<Prompt,4> prompts_{};
    std::array<State,2> states_{};
    std::array<GeoVisualDraw,2> draws_{};
    GeoVisualDraw prompt_{};
    unsigned promptOwner_=0,random_=0x54494e4bu;
    float promptAge_=0,labelAlpha_=0;
    bool promptDown_=false;
};
}
