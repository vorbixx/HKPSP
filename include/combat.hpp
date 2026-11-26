#pragma once
#include "hero.hpp"
#include <array>
#include <cstdint>

namespace hk3ds
{
constexpr std::size_t nailVariantCount = 5;
constexpr std::size_t maxNailVertices = 16;
struct NailShape
{
    Vec2 offset{}, scale{};
    std::uint32_t layerMask = 0, count = 0;
    char clip[48]{};
    std::array<Vec2,maxNailVertices> vertices{};
};
class NailBank
{
public:
    bool load(const char* path);
    bool ready() const { return loaded_; }
    std::array<NailShape,nailVariantCount> shapes{};
private:
    bool loaded_ = false;
};
std::size_t nailVariant(const NailState& nail);
Vec2 nailPoint(const Hero& hero, const NailShape& shape, Vec2 local);
bool polygonOverlapsBox(const Vec2* vertices, std::size_t count, Rect box);
struct CombatCircle { Vec2 center{}; float radius=0; };
bool validHitCircle(const CombatCircle&);
bool validHitTriangle(const std::array<Vec2,3>&);
bool validHitSegment(const std::array<Vec2,2>&);
bool circleOverlapsBox(const CombatCircle&,Rect);
bool polygonOverlapsCircle(const Vec2*,std::size_t,const CombatCircle&);
bool nailOverlaps(const Hero& hero, const NailShape& shape, Rect box);
bool nailOverlapsCircle(const Hero&,const NailShape&,const CombatCircle&);
bool nailOverlapsQuad(const Hero&,const NailShape&,const std::array<Vec2,4>&);
bool nailOverlapsPolygon(const Hero&,const NailShape&,const Vec2*,unsigned count);

enum class HitOutcome { Ignored, Blocked, Damaged, Killed, SpecialDeath };
enum class AttackType { Nail, Spell, Generic };
struct NailHit
{
    Strike direction = Strike::Right;
    int damage = 0;
    float multiplier = 1;
    bool ignoreInvulnerable = false;
    AttackType type = AttackType::Nail;
    float recoilMultiplier = 1;
};
struct NailContact;
// Synchronous HealthManager acceptance callback, before HP/death mutation.
// A null contact denotes a non-nail source; magnitude and live actor state
// belong to that source's adapter, independently of the damage multiplier.
struct AcceptedHit
{
    void* receiver = nullptr;
    void (*callback)(void*, const NailHit&, const NailContact*) = nullptr;
    const NailContact* contact = nullptr;
};
// HealthManager's nail-hit branch. Per-enemy configuration must come from
// that enemy's export; the diagnostic fixtures are explicitly separate.
struct EnemyHealth
{
    int hp = 0, enemyType = 0, invincibleFromDirection = 0;
    bool invincible = false, dead = false, damageOverride = false;
    bool spellVulnerable = false; // Original "Spell Vulnerable" tag bypasses blocking for spells.
    bool alternateHitAnimation = false, specialDeath = false;
    bool preventInvincibleEffect = false, dontClinkGates = false;
    float evasion = -1;
    void update(float dt) { if (!dead) evasion-=dt; }
    std::uint32_t hits = 0, blocks = 0;
    Strike lastDirection = Strike::Right;
    bool blocksDirection(Strike direction) const;
    HitOutcome hit(const NailHit& hit, Vitals& heroVitals, const Charms& charms,
                   const AcceptedHit& accepted = {});
};
struct CombatObject
{
    Rect bounds{};
    const std::array<Vec2,4>* hitPolygon = nullptr; // Optional static trigger geometry, owner-managed.
    // Convex pieces of one logical receiver; gaps between pieces are not hittable.
    const std::array<Vec2,3>* hitTriangles = nullptr;
    unsigned hitTriangleCount = 0;
    const std::array<Vec2,2>* hitSegments = nullptr;
    unsigned hitSegmentCount = 0;
    const CombatCircle* hitCircles = nullptr; // Union with triangle pieces, one receiver.
    unsigned hitCircleCount = 0;
    // One collider: circle replaces bounds. Two: collider 0 uses bounds and
    // collider 1 uses this circle. Both deliver contacts to this health owner.
    const CombatCircle* hitCircle = nullptr; // Owner-managed world geometry.
    const char* name = "";
    int parent = -1;
    unsigned layer = 11, colliderCount = 1;
    bool colliderEnabled = true, hasHealth = false;
    bool receivesAreaHits = false; // Non-health source Hit/trigger receivers, such as breakable decor.
    bool nonBouncer = false, bigBouncer = false, bounceShroom = false;
    EnemyHealth health{};
    void* receiver = nullptr;
    void (*onHit)(void*, const NailHit&, HitOutcome) = nullptr;
    void* acceptedHitReceiver = nullptr;
    void (*onAcceptedHit)(void*, const NailHit&, const NailContact*) = nullptr;
    HitOutcome hit(const NailHit& attack, Vitals& vitals, const Charms& charms,
                   const NailContact* contact = nullptr)
    { return health.hit(attack,vitals,charms,{acceptedHitReceiver,onAcceptedHit,contact}); }
};
struct CombatEvents
{
    std::uint32_t takeDamage = 0, dealtDamage = 0, hitLanded = 0;
    std::uint32_t tookDamage = 0, blockedHit = 0, zeroHP = 0, mushroomBounce = 0;
    std::uint32_t clink = 0, nailCallbacks = 0, damageCallbacks = 0, healthCalls = 0;
};
enum class TriggerPhase { Enter, Stay };
struct NailContact
{
    std::size_t object=0, variant=0;
    unsigned collider=0;
    TriggerPhase phase=TriggerPhase::Enter;
};
enum class CombatPhase { NailReaction, DamageCallback, LimiterAdd, TakeDamage, HealthHit, LimiterUpdate };
struct CombatTrace
{
    CombatPhase phase{};
    NailContact contact{};
    std::size_t target=0, sentCount=0;
    bool accepted=false;
    HitOutcome outcome=HitOutcome::Ignored;
};
// Read-only diagnostic AABBs of the registered geometry; never mutates contacts.
std::size_t copyDiagnosticBounds(const CombatObject*,std::size_t,Rect*,std::size_t);
class CombatScene
{
    friend struct SimulationStateHash;
public:
    // A native room capacity, not an observed Hollow Knight limit. No allocations
    // occur during contacts; oversized rooms are rejected, never truncated.
    static constexpr std::size_t capacity = 128;
    bool bind(CombatObject* objects, std::size_t count, const NailBank* bank);
    void update(const Hero& hero, float dt);
    void fixedUpdate(Hero& hero);
    void charmEffects(Hero& hero,float dt);
    // A projectile may pierce distinct targets, but each health owner is hit once.
    void spellHit(Rect sweptBounds, Strike direction, std::array<bool,capacity>& hit, Hero& hero, int damage);
    void superDashHit(Hero& hero);
    // The physics adapter snapshots all touching colliders before gameplay can
    // disable them. These phase calls also support measured source schedules.
    void nailReaction(const NailContact& contact, Hero& hero);
    void damageCallback(const NailContact& contact, Hero& hero);
    void updateLimiter(std::size_t variant, bool enabled);
    void updateHealth(std::size_t object, float dt);
    std::size_t sentCount(std::size_t variant) const;
    void* traceContext=nullptr;
    void (*trace)(void*, const CombatTrace&)=nullptr;
    const NailBank* bank() const { return bank_; }
    std::size_t diagnosticBounds(Rect* out,std::size_t capacity) const {
        return copyDiagnosticBounds(objects_,count_,out,capacity);
    }
    CombatEvents events{};
private:
    void areaHit(Rect bounds,const NailHit& attack,std::array<bool,capacity>& hit,Hero& hero,bool superDashContacts=false);
    void sendDamage(const NailContact& contact, Hero& hero);
    void mark(CombatPhase phase, const NailContact& contact, std::size_t target,
              bool accepted=false, HitOutcome outcome=HitOutcome::Ignored);
    void hitResponders(std::size_t object, Hero& hero, const NailContact& contact);
    CombatObject* objects_ = nullptr;
    unsigned observedThorns_=0;
    float hatchlingTime_=0;
    std::size_t count_ = 0;
    const NailBank* bank_ = nullptr;
    std::array<std::array<bool,capacity>,nailVariantCount> sent_{};
    std::array<std::array<std::array<bool,2>,capacity>,nailVariantCount> touching_{};
    std::array<int,nailVariantCount> observed_{{-1,-1,-1,-1,-1}};
};
}
