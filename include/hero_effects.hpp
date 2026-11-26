#pragma once
#include "animation.hpp"
#include <array>

namespace hk3ds {
struct HeroEffectConfig {
    unsigned owner=0,shape=0,tiles=0,flags=0;
    // duration/delay; life/speed/size/rotation ranges; gravity/radius/cone/arc;
    // clamp; offset XY; shape and rotation XYZ bases; force/velocity/spin;
    // eight samples each: size low/high, alpha, rate; box XYZ; cone length.
    // flags bit 2: animated fling; v49=fps, v82/83=inclusive batch count,
    // v8/9 and v84/85=the two world-space launch cones.
    std::array<float,86> v{};
};
struct HeroEffectParticle {
    Vec2 position{},velocity{},force{},module{};
    PresentationPosition history;
    const Clip* clip=nullptr;
    float age=0,life=1,size=1,angle=0,spin=0,mix=0,velocityZ=0,forceZ=0,moduleZ=0;
    unsigned emitter=0;
    bool active=false;
    Vec2 renderPosition(PresentationTime time) const {return history.sample(position,time);}
};
struct HeroDamageFlash {
    const Clip* clip=nullptr;
    Vec2 position{},scale{};
    float angle=0,age=0,alpha=0;
    bool active=false;
};
// Ordinary-route presentation. Counters come only from accepted gameplay actions.
class HeroEffects {
    friend struct SimulationStateHash;
public:
    static constexpr unsigned capacity=512;
    static constexpr unsigned emitterCount=8;
    bool load(const char*,const AnimationBank&);
    void reset();
    void update(float seconds,const Hero&);
    const std::array<HeroEffectParticle,capacity>& particles() const {return particles_;}
    const std::array<HeroEffectConfig,emitterCount>& configs() const {return configs_;}
    const std::array<unsigned,6>& emitted() const {return emitted_;}
    const std::array<unsigned,2>& flung() const {return flung_;}
    unsigned activeCount() const;
    unsigned dropped() const {return dropped_;}
    float drawSize(const HeroEffectParticle&) const;
    float opacity(const HeroEffectParticle&) const;
    const HeroDamageFlash& damageFlash() const {return flash_;}
private:
    struct Emission {Vec2 origin{};float age=0,remainder=0,mirror=1;bool active=false;};
    void start(unsigned,const Hero&);
    void spawn(unsigned,Vec2,float);
    float random(float,float);
    std::array<HeroEffectConfig,emitterCount> configs_{};
    std::array<std::array<const Clip*,7>,emitterCount> clips_{};
    std::array<HeroEffectParticle,capacity> particles_{};
    std::array<Emission,emitterCount> emissions_{};
    std::array<unsigned,6> emitted_{};
    std::array<unsigned,2> flung_{};
    std::array<float,7> flashConfig_{}; // offset XY, scale XY, duration, rotation min/max
    // Three unweighted Hermite keys (time/value/in/out) for scale X/Y and alpha.
    std::array<std::array<std::array<float,4>,3>,3> flashCurves_{};
    float flashRemaining_=0;
    const Clip* flashClip_=nullptr;
    HeroDamageFlash flash_{};
    unsigned jump_=0,dash_=0,damage_=0,dropped_=0,seed_=1,live_=0;
    bool ready_=false;
};
}
