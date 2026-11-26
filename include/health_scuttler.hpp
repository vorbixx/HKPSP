#pragma once
#include "health_cocoon_fx.hpp"

namespace hk3ds {
enum class ScuttlerPhase { InAir, Landing, Running, Bouncing, Healing, Spent };
struct HealthScuttler {
    Vec2 position{},velocity{};
    PresentationPosition history;
    GeoVisualDraw draw;
    float scale=1,maxSpeed=0,age=0,timer=0;
    ScuttlerPhase phase=ScuttlerPhase::Spent;
    bool alive() const { return phase!=ScuttlerPhase::Healing&&phase!=ScuttlerPhase::Spent; }
    Rect bounds() const;
    Vec2 renderPosition(PresentationTime time) const { return history.sample(position,time); }
};

// sharedassets6:436 / ScuttlerControl. Two room-owned bodies, no dynamic pool.
class HealthScuttlers {
    friend struct SimulationStateHash;
public:
    static constexpr unsigned capacity=2;
    bool bind(const AnimationBank&,std::uint32_t seed=0x53435554u);
    void reset();
    void spawn(Vec2 origin,float depth);
    void fixedUpdate(float dt,const World&);
    void update(float dt,Hero&,const NailBank&,DecorEffects* effects=nullptr);
    bool hit(unsigned index);
    void unload(Vitals&); // Source Heal also grants pending health on scene unload.
    const std::array<HealthScuttler,capacity>& actors() const { return actors_; }
    const HealthCocoonFx& effects(unsigned i) const { return effects_[i]; }
    static constexpr unsigned healCapacity=20;
    const std::array<GeoVisualDraw,healCapacity+capacity>& draws(PresentationTime) const;
    unsigned activeMotes() const;
    unsigned bounceSerial() const { return bounces_; }
    unsigned deathSerial() const { return deaths_; }
    unsigned healSerial() const { return heals_; }
    float screenFlashAge(unsigned i=0) const { return flashAges_[i]; }
    float screenFlashStrength(unsigned i=0) const;
    bool spawned() const { return spawned_; }
private:
    float random(float low,float high);
    void run(HealthScuttler&);
    struct HealParticle {
        enum class Phase { Inactive, Fall, Decelerate, Attract, Flash } phase=Phase::Inactive;
        Vec2 position{},velocity{};
        PresentationPosition history;
        GeoVisualDraw draw;
        float timer=0,speed=0;
        unsigned owner=0;
    };
    std::array<HealParticle,healCapacity> motes_{};
    std::array<GeoVisualDraw,capacity> splats_{};
    mutable std::array<GeoVisualDraw,healCapacity+capacity> drawList_{};
    const AnimationBank* bank_=nullptr;
    std::array<HealthScuttler,capacity> actors_{};
    std::array<HealthCocoonFx,capacity> effects_{};
    std::uint32_t seed_=1,random_=1;
    unsigned bounces_=0,deaths_=0,heals_=0;
    std::array<float,capacity> flashAges_{{1,1}};
    bool spawned_=false;
};
}
