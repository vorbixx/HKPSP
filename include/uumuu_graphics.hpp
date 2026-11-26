#pragma once
#include "renderer.hpp"
#include "uumuu_art.hpp"

namespace hk3ds {
// GPU adapter only. The room retains ownership of encounter scheduling and
// animation actors, and must release them before replacing these borrowed banks.
class UumuuGraphics {
public:
    // futureBytes covers the room/other banks still to be loaded concurrently.
    // Current resident allocations are accounted by the loader's live query.
    bool load(const char* root,std::size_t futureBytes=0);
    void release();
    const AnimationBank& bodyBank()const{return body_;}
    const AnimationBank& quirrelBank()const{return quirrel_;}
    const HeroRenderer& bodyRenderer()const{return bodyRenderer_;}
    const HeroRenderer& quirrelRenderer()const{return quirrelRenderer_;}
    const AnimationBank& gateBank()const{return gates_;}
    const HeroRenderer& gateRenderer()const{return gateRenderer_;}
    const AnimationBank& effectsBank()const{return effects_;}
    const HeroRenderer& effectsRenderer()const{return effectsRenderer_;}
    bool ready()const{return bodyRenderer_.ready()&&quirrelRenderer_.ready()&&gateRenderer_.ready()&&effectsRenderer_.ready();}
private:
    AnimationBank body_,quirrel_,gates_,effects_;
    HeroRenderer bodyRenderer_,quirrelRenderer_,gateRenderer_,effectsRenderer_;
};
}
