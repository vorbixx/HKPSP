#pragma once
#include "animation.hpp"
#include <array>

namespace hk3ds {
enum class FocusDrawPass { BeforeForeground, AfterForeground };
struct FocusEffectDraw {
    AnimationPlayer animation;
    Vec2 position{},scale{1,1};
    float alpha=1;
    bool active=false,screenBlend=false,followHero=true;
    FocusDrawPass pass=FocusDrawPass::BeforeForeground;
};
struct FocusParticle {
    PresentationPosition history;
    Vec2 position{},velocity{};
    const Clip* clip=nullptr;
    float age=0,life=1,size=1,fade=0;
    bool active=false;
};
class FocusEffects {
    friend struct SimulationStateHash;
public:
    static constexpr unsigned capacity=128,maxSprites=5,maxEmitters=4;
    bool load(const char* path,const AnimationBank& bank);
    void reset();
    void update(float seconds,const Hero& hero);
    const std::array<FocusEffectDraw,maxSprites>& draws() const {return draws_;}
    const std::array<FocusParticle,capacity>& particles() const {return particles_;}
    unsigned activeCount() const {return live_;}
    unsigned dropped() const {return dropped_;}
    static float opacity(const FocusParticle& particle);
    float healFlash() const;
private:
    struct Sprite {const Clip* clip=nullptr;std::array<float,6> values{};unsigned kind=0;float age=0;};
    struct Emitter {const Clip* clip=nullptr;std::array<float,15> values{};unsigned shape=0;float remainder=0;};
    void play(unsigned kind);
    void spawn(Emitter& emitter,const Hero& hero);
    float random(float low,float high);
    std::array<Sprite,maxSprites> sprites_{};
    std::array<Emitter,maxEmitters> emitters_{};
    std::array<FocusEffectDraw,maxSprites> draws_{};
    std::array<FocusParticle,capacity> particles_{};
    FocusPhase previous_=FocusPhase::None;
    float flashAge_=.37f;
    Vec2 flashOrigin_{};
    unsigned spriteCount_=0,emitterCount_=0,live_=0,dropped_=0,seed_=1;
    bool ready_=false;
};
}
