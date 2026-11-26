#pragma once
#include "enemy.hpp"
#include <array>

namespace hk3ds
{
class AudioVoices;
enum class EnemyBKind : unsigned { WanderingHusk, HuskHornhead, LeapingHusk, Goam, FungCrawler, Count };
struct EnemyBRegistration { EnemyBKind kind; const char* prefab; const char* prefix; };
// Append-only group registry; scene conversion can resolve the original prefab.
inline constexpr std::array<EnemyBRegistration,4> enemyBRegistry{{
    {EnemyBKind::WanderingHusk,"Zombie Runner","wandering"},
    {EnemyBKind::HuskHornhead,"Zombie Hornhead","hornhead"},
    {EnemyBKind::LeapingHusk,"Zombie Leaper","leaping"},
    {EnemyBKind::Goam,"Worm","goam"}
}};
const EnemyBRegistration* findEnemyB(const char* originalPrefab);
struct EnemyBConfig
{
    unsigned hp=0, damage=0, smallGeo=0;
    Rect body{};
    float speed=0, chaseSpeed=0, detectX=0, detectY=0, wait=0;
    float jumpX=0, jumpY=0, gravity=0, recoilSpeed=0, recoilDuration=0;
    float hideTime=0, upTime=0, downTime=0, exposedTime=0;
    // Original Worm sprite colliders: Burst 5, Idle 3, Retract 5 frames.
    std::array<Rect,13> goamBodies{};
};
using EnemyBConfigs = std::array<EnemyBConfig,4>;
bool loadEnemyBConfigs(const char* path,EnemyBConfigs& destination);
enum class EnemyBState { Patrol, Turn, Windup, Attack, Air, Land, Hidden, Rise, Exposed, Retract, Dead, Removed, Waiting, Stopped, Idle };

// Physics/combat owner. Animation and interpolated poses are consumed by adapters.
class EnemyB
{
    friend struct SimulationStateHash;
public:
    bool bind(EnemyBKind,const EnemyBConfig&,const AnimationBank&,CombatObject&,Vec2,float facing=1,float rotation=0,float authoredScaleY=1);
    void fixedUpdate(float dt,Vec2 hero,const World&);
    // Walker.Start/UpdateWaitingForConditions: latch the first camera approach.
    void updateStartCondition(Vec2 camera,float depth);
    // Native Walker.Update owns bored-pause timers independently of physics.
    void updateWalker(float dt,Vec2 camera,float depth,EnemyRandom,const World&);
    void contactHero(Hero&) const;
    void emitSounds(AudioVoices&,std::uint32_t owner,std::uint32_t random);
    bool active() const { return object_!=nullptr; }
    bool alive() const { return active() && state!=EnemyBState::Dead && state!=EnemyBState::Removed; }
    bool visible() const { return active() && state!=EnemyBState::Hidden && state!=EnemyBState::Removed; }
    // Worm Control "Start Down" false: Initialise goes straight to Up (Idle clip, collider on).
    void startUp();
    bool harmful() const;
    Rect bounds() const;
    float artScale() const;
    Vec2 renderPosition(PresentationTime time) const { return presentation_.sample(position,time); }
    unsigned takeSmallGeo() { const auto count=pendingGeo_;pendingGeo_=0;return count; }
    EnemyBKind kind() const { return kind_; }
    std::uint32_t walkingPlaySerial() const { return walkingPlaySerial_; }
    Vec2 position{},velocity{};
    Vec2 geoPosition{};
    float scaleX=1;
    EnemyBState state=EnemyBState::Removed;
    AnimationPlayer animation;
    Contacts contacts{};
    std::uint32_t injurySerial=0,deathSerial=0,attackSerial=0,emergeSerial=0;
    std::uint32_t anticipationSerial=0;
    bool killFreezeRequested=false;
private:
    static void receive(void*,const NailHit&,HitOutcome);
    void hit(const NailHit&,HitOutcome);
    const Clip* clip(const char*) const;
    void enter(EnemyBState);
    void syncBounds();
    EnemyBKind kind_=EnemyBKind::WanderingHusk;
    EnemyBConfig config_{};
    const AnimationBank* bank_=nullptr;
    CombatObject* object_=nullptr;
    PresentationPosition presentation_;
    float clock_=0,recoil_=0,rotation_=0,authoredScaleY_=1,rotationCosine_=1,rotationSine_=0;
    unsigned pendingGeo_=0;
    std::uint32_t soundedInjury_=0,soundedDeath_=0,soundedAttack_=0;
    std::uint32_t walkingPlaySerial_=0;
    EnemyRandom walkerRandom_{};
    float walkerTime_=0;
    // Zombie Leap Left or Right?: (Hero X - Self X) * 1.25, latched before the anticipation.
    float leapSpeed_=0;
    // Last hero position seen by fixedUpdate; TOOK DAMAGE faces the hero (Zombie Swipe Left or Right?).
    Vec2 hero_{};
    bool heroKnown_=false;
    // Husk placement |scale| (Goam keeps its own authored axes): body and Attack Range scale with it.
    Vec2 size_{1,1};
    Rect scaled(Rect r) const { return {r.left*size_.x,r.bottom*size_.y,r.right*size_.x,r.top*size_.y}; }
    bool ready() const;
    void beginAttack(Vec2 hero);
};
}
