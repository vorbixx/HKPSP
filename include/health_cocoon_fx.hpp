#pragma once

#include "geo_visual.hpp"
#include <array>
#include <cstdint>
#include <cstdio>

namespace hk3ds {

// HCC3 stores the source prefab values alongside the cocoon transform.
struct HealthCocoonFxConfig {
    struct Fling {
        unsigned minAmount,maxAmount;
        Vec2 variation;
        float minSpeed,maxSpeed,minAngle,maxAngle;
    };
    Fling spatter{50,50,{1,1},5,20,100,440};
    Fling glob{8,10,{1.5f,1.5f},2,8,100,440};
    float capForce=10,capMass=1,capGravity=1,globGravity=.9f,spatterGravity=1;
    float bounceFactor=.4f,bounceThreshold=1,spinFactor=-7.5f,angularDamping=.05f;
    float spatterScaleMin=1.1f,spatterScaleMax=1.4f,splashScaleMin=1.2f,splashScaleMax=1.3f;
    float globScaleMin=.6f,globScaleMax=1.6f;
    Vec2 capOffset{.28f,2.31f},capScale{1,1};
    float capDepth=.01f;
    float gravity=-60,capInertia=.53091734f;
    bool read(std::FILE*);
    bool valid() const;
};

// Cosmetic fling pool used only after HealthCocoon accepts its first nail hit.
// Source amounts cap the pool at 50 Spatter Blue plus 10 Blue Glob instances
// and the single Health Cocoon cap rigidbody.
class HealthCocoonFx {
    friend struct SimulationStateHash;
public:
    static constexpr unsigned capacity=61;
    static constexpr unsigned particleCapacity=60;

    bool bind(const AnimationBank&,const char* spatterClip,const char* globClip,
              const char* capClip,
              std::uint32_t seed=0x48434631u);
    void reset();
    bool configure(const HealthCocoonFxConfig&);
    void setDepth(float depth) { depth_=depth; }
    void setCapTransform(Vec2 position,Vec2 scale) { capPosition_=position;capScale_=scale;hasCapTransform_=true; }
    void breakAt(Vec2 origin,float depth,Vec2 nailPosition);
    void burstAt(Vec2 origin,float depth);
    static void breakCallback(void* context,Vec2 origin,Strike direction,Vec2 nailPosition);
    void fixedUpdate(float seconds,const World&);
    const std::array<GeoVisualDraw,capacity>& draws(PresentationTime) const;
    unsigned activeCount() const { return activeCount_; }
    bool invariant() const;

private:
    struct Particle {
        Vec2 position{},velocity{};
        float depth=0,age=0,lifetime=0,phaseElapsed=0,scaleModifier=1;
        float angularVelocity=0;
        unsigned physicsSteps=0;
        bool active=false,landed=false,collisionInitialized=false;
        PresentationPosition history;
        GeoVisualDraw draw;
    };
    float random(float low,float high);
    unsigned randomCount(unsigned low,unsigned high);
    void emit(const Clip*,unsigned amount,Vec2 origin,Vec2 variation,
              float minSpeed,float maxSpeed,float angleLow,float angleHigh);

    const AnimationBank* bank_=nullptr;
    const Clip* spatterClip_=nullptr,*globClip_=nullptr;
    const Clip* spatterImpactClip_=nullptr;
    const Clip* capClip_=nullptr;
    const Clip* globLandClip_=nullptr,*globWobbleClip_=nullptr;
    std::array<Particle,capacity> particles_{};
    mutable std::array<GeoVisualDraw,capacity> drawList_{};
    std::uint32_t seed_=0x48434631u,random_=0x48434631u;
    unsigned activeCount_=0;
    float depth_=0;
    HealthCocoonFxConfig config_{};
    Vec2 capPosition_{};
    Vec2 capScale_{1,1};
    bool capActive_=false;
    bool hasCapTransform_=false;
};

}
