#pragma once
#include "animation.hpp"
#include "combat.hpp"
#include "geo.hpp"

namespace hk3ds {
class DecorEffects;
struct GeoChipProfile {
    unsigned minimum=3,maximum=5;
    float speedMin=15,speedMax=20,angleMin=20,angleMax=40;
};
struct GeoChipConfig {
    std::array<GeoChipProfile,4> profiles{{{3,5,15,20,20,40},{3,5,15,20,100,140},
        {3,5,15,20,70,110},{8,12,20,28,60,120}}};
    float variationX=.25f,variationY=.25f,scaleMin=1.25f,scaleMax=2,blackChance=1.f/3,
        startZ=.019f,zRange=.009f,angularDamping=.05f,gravity=-60,mass=1,
        width=.09375f,height=.109375f,offsetY=-.0078125f,bounce=.5f,threshold=1,wait=8,shrink=1;
};
struct GeoRockRecord {
    std::uint32_t owner=0,variant=0,hits=0;
    Vec2 position{};
    float depth=0;
    Vec2 scale{1,1};
    Rect collider{};
    char idle[48]{},gleam[48]{},broken[48]{};
};
struct GeoRockConfig {
    static constexpr unsigned capacity=16;
    std::array<GeoRockRecord,capacity> rocks{};
    unsigned count=0;
    GeoChipConfig chips{};
    bool load(const char* path);
};
struct GeoVisualDraw {
    AnimationPlayer player;
    Vec2 position{},scale{1,1};
    // Source Small Air/Idle have no SetFlashing animation events.
    float depth=0,flash=0,rotation=0;
    bool visible=false;
};
enum class GeoRockPhase { Idle, Gleam, Recoil, ReturnRecoil, BreakWait, Broken };
struct GeoRockVisual {
    GeoVisualDraw draw;
    GeoRockPhase phase=GeoRockPhase::Idle;
    unsigned hits=0,lastAttack=0;
    float timer=0,jitterClock=0;
    Vec2 recoil{},recoilOrigin{};
};
struct GeoChip {
    GeoVisualDraw draw;
    Vec2 position{},velocity{};
    PresentationPosition history;
    float spin=0,scale=1,sleepTime=0,offscreenTime=0;
    bool asleep=false;
    Vec2 renderPosition(PresentationTime t) const {return history.sample(position,t);}
};
// Cosmetic observers only. No combat receivers, terrain, payouts or hero mutation.
class GeoVisuals {
    friend struct SimulationStateHash;
public:
    static constexpr unsigned chipCapacity=192;
    bool bind(const AnimationBank&,const GeoRockConfig&,std::uint32_t cosmeticSeed=0x47454f31);
    void reset();
    void update(float dt,const GeoBank&,const Hero&,const NailBank&,DecorEffects* effects=nullptr,
                const World& world={},const Vec2* camera=nullptr);
    const GeoVisualDraw& coin(unsigned i) const { return coins_[i].draw; }
    const GeoRockVisual& rock(unsigned i) const { return rocks_[i]; }
    unsigned rockCount() const { return config_.count; }
    unsigned rockOwner(unsigned i) const { return config_.rocks[i].owner; }
    const std::array<GeoChip,chipCapacity>& chips() const {return chips_;}
    unsigned chipCount() const;
private:
    void emitChips(Vec2 origin,Strike direction,bool broken);
    void updateChips(float dt,const World&,const Vec2* camera);
    struct CoinVisual { GeoVisualDraw draw; unsigned spawn=0,collision=0; };
    const AnimationBank* bank_=nullptr;
    GeoRockConfig config_{};
    std::array<CoinVisual,GeoBank::capacity> coins_{};
    std::array<GeoRockVisual,GeoRockConfig::capacity> rocks_{};
    std::array<GeoChip,chipCapacity> chips_{};
    std::uint32_t random_=0x47454f31,seed_=0x47454f31;
    std::uint32_t chipRandom_=1;
    float random(float lo,float hi);
    float chipRandom(float lo,float hi);
};
}
