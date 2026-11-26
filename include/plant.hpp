#pragma once
#include "room_visual.hpp"
#include "enemy.hpp"
#include "breakable.hpp"
#include <array>
namespace hk3ds
{
enum class PlantHitDirection { Right, Up, Left, Down };
struct PlantDebrisPose { float z=0,rotation=0;bool active=false; };
struct PlantBreakConfig
{
    // Effective offset after original Start (including the root scale sign).
    float angleOffset=0;
    std::array<PlantDebrisPose,2> debris{};
};
enum class PlantBreakEvent { StaticParts, DebrisActivated, DebrisRotated, CameraShake };
// Selected four-plant activation contract. No particle solver or shared Hit FX.
// Random supplies SetZRandom samples; it does not reproduce Unity's global RNG.
class PlantBreakState
{
    friend struct SimulationStateHash;
public:
    using Notify=void (*)(void*,PlantBreakEvent,unsigned,const PlantBreakState&);
    PlantBreakState()=default;
    PlantBreakState(const PlantBreakState&)=delete;
    PlantBreakState& operator=(const PlantBreakState&)=delete;
    bool bind(const PlantBreakConfig&,EnemyRandom,void* context=nullptr,Notify notify=nullptr);
    void reset();
    bool hit(PlantHitDirection);
    const BreakableActivation& activation() const {return state_.activation();}
    const std::array<PlantDebrisPose,2>& debris() const {return debris_;}
private:
    static void receive(void*,BreakableAction,const BreakableActivation&);
    void emit(PlantBreakEvent,unsigned index=0);
    PlantBreakConfig config_{};
    BreakableState state_{};
    EnemyRandom random_{};
    std::array<PlantDebrisPose,2> debris_{};
    float offset_=0;
    bool bound_=false;
    void* context_=nullptr;
    Notify notify_=nullptr;
};
struct PlantKey {float time=0;std::uint32_t sprite=0;};
struct PlantClip
{
    float duration=0;
    unsigned count=0;
    std::array<PlantKey,64> keys{};
    std::uint32_t sprite(float normalized) const;
};
struct PlantConfig
{
    std::uint32_t owner=0,authored=0,clip=0;
    bool randomStart=true;
    DecorConfig collision{};
    PlantBreakConfig breakage{};
};
class BreakablePlant
{
    friend struct SimulationStateHash;
public:
    bool bind(const PlantConfig&,CombatObject&,EnemyRandom,void* context=nullptr,
              BreakableDecor::HitEffect effect=nullptr);
    bool broken() const {return state_.activation().broken;}
    float alpha(unsigned owner) const;
    const PlantBreakState& state() const {return state_;}
private:
    static void receive(void*,const NailHit&,HitOutcome);
    PlantConfig config_{};
    PlantBreakState state_{};
    CombatObject* object_=nullptr;
    void* context_=nullptr;
    BreakableDecor::HitEffect effect_=nullptr;
};
struct PlantBank
{
    unsigned count=0,passiveCount=0,clipCount=0,spriteCount=0,drawCount=0;
    // Only count entries participate in combat; the tail is presentation only.
    unsigned animationCount() const {return count+passiveCount;}
    std::array<PlantConfig,7> plants{};
    std::array<PlantClip,2> clips{};
    bool load(const char* path);
};
// Sprite-only Animator observations; no moving Transform or physics position.
struct PlantAnimation
{
    float phase=0,sample=0;
    bool started=false,sampled=false,evaluated=false,wasVisible=false;
    void advance(float scaledSeconds,bool visible,const PlantClip& clip,EnemyRandom random,bool randomStart=true);
};
class RoomPlants
{
    friend struct SimulationStateHash;
public:
    bool bind(const PlantBank& bank,RoomVisualBank& room,EnemyRandom random);
    void restart();
    void present(float scaledSeconds,float cameraX,float cameraY);
    bool ready() const {return bank_!=nullptr;}
    const PlantAnimation& animation(unsigned index) const {return states_[index];}
private:
    const PlantBank* bank_=nullptr;
    RoomVisualBank* room_=nullptr;
    EnemyRandom random_;
    unsigned passiveSeed_=0x504c414eu;
    static float passiveUnit(void*);
    std::array<unsigned,7> draws_{};
    std::array<PlantAnimation,7> states_{};
};
}
