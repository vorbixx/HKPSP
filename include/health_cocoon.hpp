#pragma once
#include "geo_visual.hpp"
#include "health_cocoon_fx.hpp"
#include <cstdint>

namespace hk3ds {

// HCC3 includes source FX physics and the break's disabled terrain colliders. Trigger bounds are the
// source Health Cocoon's nail-trigger bounds in room coordinates.
struct HealthCocoonConfig {
    std::uint32_t owner=0;
    Vec2 position{};
    float depth=0;
    Vec2 scale{1,1};
    Rect nailBounds{};
    Vec2 splatPosition{};
    float splatDepth=0;
    Vec2 splatScale{1,1};
    std::array<std::uint32_t,2> disabledColliders{};
    HealthCocoonFxConfig fx{};
    bool load(const char* path);
};

class HealthCocoon {
    friend struct SimulationStateHash;
public:
    using BreakEffect=void(*)(void*,Vec2,Strike,Vec2);

    bool bind(const AnimationBank&,const HealthCocoonConfig&,
              std::uint32_t cosmeticSeed=0x48434f31u);
    bool bind(const AnimationBank&,const char* configPath,
              std::uint32_t cosmeticSeed=0x48434f31u);
    void reset();
    void update(float dt,const Hero&,const NailBank&,
                BreakEffect effect=nullptr,void* effectContext=nullptr);

    const GeoVisualDraw& visual() const { return visual_; }
    const GeoVisualDraw& splatVisual() const { return splat_; }
    bool broken() const { return broken_; }
    const HealthCocoonFxConfig& fxConfig() const { return config_.fx; }
    const std::array<std::uint32_t,2>& disabledColliders() const { return config_.disabledColliders; }
    std::uint32_t moveSerial() const { return moveSerial_; }
    std::uint32_t breakSerial() const { return breakSerial_; }

private:
    float random(float low,float high);
    void startIdleWait();

    const AnimationBank* bank_=nullptr;
    HealthCocoonConfig config_{};
    GeoVisualDraw visual_{},splat_{};
    std::uint32_t random_=0x48434f31u,seed_=0x48434f31u;
    std::uint32_t lastAttack_=0,moveSerial_=0,breakSerial_=0;
    float wait_=0,moveElapsed_=0,splatElapsed_=0;
    bool hasAttack_=false,broken_=false;
};

}
