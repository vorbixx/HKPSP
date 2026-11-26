#pragma once
#include "animation.hpp"
#include "presentation.hpp"
#include <array>

namespace hk3ds {
struct HeroDeathParticle {
    Vec2 position{},velocity{};
    PresentationPosition history;
    const Clip* clip=nullptr;
    float age=0,lifetime=0,size=1,angle=0,spin=0,accelerationY=0,sizeCurveMix=0;
    unsigned kind=0;
    bool active=false;
    Vec2 renderPosition(PresentationTime time) const { return history.sample(position,time); }
    float drawSize() const;
};

// Presentation only: never changes the hero, room clock, retry or Shade.
class HeroDeathPresentation {
    friend struct SimulationStateHash;
public:
    static constexpr unsigned capacity=640;
    void reset();
    void begin(const Hero&,const AnimationBank&);
    void update(float seconds);
    void fixedUpdate(float seconds,const World&);
    const std::array<HeroDeathParticle,capacity>& particles() const { return particles_; }
    unsigned activeCount() const;
    bool active() const { return active_; }
    float elapsed() const { return elapsed_; }
    bool bodyVisible() const { return active_&&!body.complete; }
    float flashOpacity(bool rays=false) const;
    float flashScale(bool rays=false) const;
    Vec2 origin{};
    float flashRotation=0;
    AnimationPlayer body;
    AnimationPlayer flash,flashRays;
    bool facingRight=false;
private:
    HeroDeathParticle* spawn(unsigned kind,Vec2 position,Vec2 velocity,float lifetime,float size);
    float random(float low,float high);
    std::array<HeroDeathParticle,capacity> particles_{};
    const AnimationBank* bank_=nullptr;
    float elapsed_=0,physicsElapsed_=0;
    std::array<unsigned,4> emitted_{};
    std::uint32_t seed_=0;
    bool active_=false,nail_=false,head_=false;
};
}
