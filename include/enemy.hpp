#pragma once
#include "presentation.hpp"
#include "animation.hpp"
#include "combat.hpp"
#include <cstdint>

namespace hk3ds
{
// Only the validated Tutorial_01 Crawler branch. Geometry and settings are
// loaded from the private export; there is no substitute enemy asset.
struct CrawlerConfig
{
    unsigned hp=0, damage=0, layer=0, colliderCount=0, smallGeo=0;
    Rect body{}, corpse{}, heroHurtbox{};
    float speed=0, turnDelay=0, recoilSpeed=0, recoilDuration=0;
    float gravity=0, corpseGravity=0;
    Vec2 spawnOffset{};
    PhysicsMaterial bodyMaterial{}, corpseMaterial{};
    float flingSpeed=0, bounceFactor=0, bounceThreshold=0;
    float leftWait=0, rightWait=0, leftTranslate=0, rightTranslate=0;
    bool load(const char* path);
};
struct EnemyRandom
{
    void* context=nullptr;
    float (*unit)(void*)=nullptr; // [0,1], injectable; not Unity's random sequence.
    float (*sample)(void*,float,float)=nullptr; // Optional measured RNG source.
    float (*walkerSample)(void*,float,float)=nullptr;
    float (*hatcherIdleSample)(void*,float,float)=nullptr;
    float range(float low,float high) const;
};
// Build22529139 Hatcher Baby, sharedassets59:30 Control/Chase:
// ChaseObject.OnEnter/OnFixedUpdate and FaceDirection.OnUpdate are separate.
struct HatchlingSteering
{
    friend struct SimulationStateHash;
    void start(Vec2 position,Vec2 target,Vec2& velocity,EnemyRandom);
    void fixedUpdate(float dt,Vec2 position,Vec2 target,Vec2& velocity,EnemyRandom);
    void update(float dt,Vec2 velocity,float& scaleX);
private:
    void chase(float dt,Vec2 position,Vec2 target,Vec2& velocity,EnemyRandom);
    Vec2 spread_{};
    float timer_=0,period_=0,facePause_=0;
    bool entered_=false;
};
enum class WalkState { Walk, StartLeft, StartRight, Maintain };
enum class CorpseState { Absent, InAir, PendingLandEffects, Complete, Removed };
struct WalkProbes { bool grounded=false, wall=false, missingFloor=false; };
// Sweep uses the last interpolated Transform. A successful Translate also
// synchronizes Rigidbody, as observed in source FixedUpdate enter/exit pairs.
bool translateRecoil(Vec2& rigidbody, Vec2& transform, Rect capturedBox,
                     Vec2 direction, float distance, const World& world);
class Crawler
{
    friend struct SimulationStateHash;
public:
    static bool canBind(const CrawlerConfig&,const AnimationBank&,EnemyRandom,float initialScale);
    bool bind(const CrawlerConfig& config, const AnimationBank& bank, CombatObject& object,
              Vec2 position, const World& world, EnemyRandom random, float initialScale=1);
    void update(float scaled, float unscaled, const World& world);
    void fixedUpdate(float dt, const World& world, bool resumeWalk = true);
    void afterPhysics(const World& world);
    // Called by collider callbacks before this frame's Update.
    void command(bool right, const World& world);
    void interpolate(float alpha);
    void contactHero(Hero& hero) const;
    WalkProbes probes(const World& world) const;
    bool active() const { return object_!=nullptr; }
    bool alive() const { return active() && !object_->health.dead; }
    bool visible() const { return alive() || (corpseState!=CorpseState::Absent && corpseState!=CorpseState::Removed); }
    bool recoiling() const { return recoilTime_>0; }
    Rect bounds() const;
    float artScale() const;
    Vec2 renderPosition(PresentationTime time) const
    { return alive()?presentation_.sample(position,time):corpsePresentation_.sample(corpsePosition,time); }
    Vec2 position{}, transformPosition{}, velocity{}, corpsePosition{}, corpseVelocity{};
    float scaleX=1, corpseScaleX=1;
    bool turning=false, killFreezeRequested=false;
    std::uint32_t injurySerial=0, killSerial=0;
    WalkState state=WalkState::Walk;
    CorpseState corpseState=CorpseState::Absent;
    AnimationPlayer animation;
    Contacts terrainContacts{};
private:
    static void receive(void* self, const NailHit& hit, HitOutcome outcome);
    void hit(const NailHit& hit, HitOutcome outcome);
    void enter(WalkState state, const World& world);
    void walk(const World& world);
    void syncBounds();
    void corpsePhysics(float dt, const World& world);
    PresentationPosition presentation_,corpsePresentation_;
    CrawlerConfig config_{};
    const AnimationBank* bank_=nullptr;
    CombatObject* object_=nullptr;
    EnemyRandom random_{};
    // Time.time is an engine clock, not a repeatedly rounded actor float sum.
    // Keep elapsed deadlines in double so long-lived actors do not miss a tick.
    double time_=0;
    std::array<double,4> nextTurn_{};
    float turnTime_=0, commandTime_=0, recoilTime_=0, landTime_=0, recoilMultiplier_=1;
    std::array<bool,4> entered_{};
    bool shouldTurn_=false, sweeping_=false, turnBeganInPhysics_=false, commandFresh_=false;
    Rect recoilBox_{};
    Vec2 previousPosition_{}, recoilDirection_{}, sampledDisplacement_{}, lastSamplePosition_{};
    float sampledSpeed_=0;
    unsigned sampleStep_=0, corpseContacts_=0;
    std::array<std::uint32_t,16> previousCorpseContacts_{};
    std::size_t previousCorpseCount_=0;
};
// Isolated single-kill control retained for standalone tests. Runtime and
// encounter replay use the shared GameClock for concurrent coroutine writers.
struct KillFreeze
{
    float scale=1, elapsed=0, startScale=1;
    int phase=0;
    void start(float currentScale);
    void update(float unscaled);
};
}
