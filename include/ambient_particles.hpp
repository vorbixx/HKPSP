#pragma once

#include "presentation.hpp"
#include "room_visual.hpp"
#include <array>
#include <cstdint>

namespace hk3ds
{
// World-space motes and leaves from camera-follow emitters. The draw storage
// is fixed at the largest recovered bank; update/present allocate nothing.
class AmbientParticles
{
    friend struct SimulationStateHash;
public:
    static constexpr unsigned capacity=512;
    bool load(const char* config,unsigned spriteOverride=~0u);
    unsigned spriteIndex() const {return sprite_;}
    // Select the recovered controller bank for an authored zone. Missing banks
    // leave the current configuration intact and return false.
    bool selectZone(unsigned zone);
    bool bindVisuals(const RoomVisualBank& decorVisuals,bool roomTextures=false);
    void reset(unsigned seed=1);
    void clear();
    void prewarm(Vec2 camera);
    void update(float seconds,Vec2 camera);
    void present(PresentationTime time);

    bool ready() const {return ready_;}
    unsigned count() const {return count_;}
    bool lifetimeInvariant() const;
    RoomVisualBank visuals;

private:
    static constexpr unsigned emitterCount=8,curveSamples=16,edgeCount=320;
    struct Edge {Vec2 start{},end{};float cumulative=0;}; // Plane mesh X/Z.
    struct Emitter
    {
        unsigned count=0,flags=0,sprite=0,frames=1,material=4,shape=6;
        int layer=0,order=0;
        Vec2 position{},halfExtent{};
        std::array<float,3> shapeHalf{};
        std::array<float,9> shapeBasis{};
        bool transformedShape=false;
        std::array<float,4> rotation{0,0,0,1};
        float z=0,scaleX=1,scaleY=1,scaleZ=1,shapeAngle=0,halfHeight=0;
        float rate=0,lifeMin=0,lifeMax=0,sizeMin=0,sizeMax=0,alphaGain=1;
        float speedMin=0,speedMax=0,gravityMin=0,gravityMax=0;
        Vec2 velocityMin{},velocityMax{};
        float velocityZMin=0,velocityZMax=0;
        float angleMin=0,angleMax=0,spinMin=0,spinMax=0;
        unsigned spriteCount=1;
        std::array<float,4> colorMin{},colorMax{};
        std::array<std::array<float,4>,curveSamples> color{};
        std::array<float,curveSamples> size{},sizeMaxCurve{};
    };
    struct Particle
    {
        Vec2 position{},velocity{},birthCamera{};
        PresentationPosition history;
        std::array<float,4> startColor{};
        float age=0,life=0,size=0,angle=0,z=0,velocityZ=0,gravity=0;
        float sizeBlend=0,cosAngle=1,sinAngle=0,spin=0;
        unsigned frame=0;
        unsigned emitter=0,birthEdge=0;
        bool alive=false;
    };
    std::array<Emitter,emitterCount> emitters_{};
    std::array<Edge,edgeCount> edges_{};
    std::array<Particle,capacity> particles_{};
    std::array<float,emitterCount> emissionClock_{};
    std::array<unsigned,emitterCount> liveByEmitter_{};
    std::array<std::uint32_t,(capacity+31)/32> freeSlots_{};
    unsigned sprite_=0,count_=0,seed_=1,activeCapacity_=0,activeEmitters_=0;
    bool ready_=false,bound_=false;
    float random(float low,float high);
    void spawn(unsigned emitter,Vec2 camera,float age=0);
    float curve(const std::array<float,curveSamples>& values,float t) const;
    std::array<float,4> colorCurve(const Emitter&,float t) const;
};
}
