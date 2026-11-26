#pragma once
#include "presentation.hpp"
#include "input.hpp"
#include "physics.hpp"
#include <cstdint>

#include "super_dash.hpp"
#include "surface_swim.hpp"

namespace hk3ds
{
enum class Actor { Idle, Running, Airborne, HardLanding, Damage, Dead };
enum class Strike { Right, Left, Up, Down };
enum class FocusPhase { None, ButtonHeld, Starting, Draining, Healing, Finishing, Cancelling };
enum class AbilityReward { VengefulSpirit, MothwingCloak, MantisClaw, MonarchWings, CrystalHeart, IsmasTear, DreamNail, ShadeCloak, ShadeSoul };
struct Abilities {
    bool dash = false, wallJump = false, doubleJump = false, shadeDash = false, vengefulSpirit = false, superDash = false, acidArmour = false, shadeSoul = false;
    unsigned spiritLevel() const { return shadeSoul?2u:(vengefulSpirit?1u:0u); }
};
struct Charms
{
    bool sprint = false, dashmaster = false, sharpShadow = false, quickSlash = false;
    bool steadyBody = false, stalwart = false, quickFocus = false, deepFocus = false;
    bool soulCatcher = false, soulEater = false, overcharmed = false;
    bool longnail = false, markOfPride = false;
    bool gatheringSwarm=false,compass=false,grubsong=false,baldurShell=false,fury=false;
    bool thorns=false,lifebloodHeart=false,shamanStone=false,glowingWomb=false;
};
struct Vitals
{
    int health = 5, maxHealth = 5, blueHealth = 0;
    int soul = 0, maxSoul = 99, reserve = 0, reserveMax = 0;
    unsigned soulGainSerial=0;
    void takeHealth(int amount);
    void heal(int amount);
    void gainSoul(const Charms& charms);
};
struct NailState
{
    bool active = false, hitbox = false, alternate = false, nextAlternate = false, wall = false;
    Strike direction = Strike::Right;
    float elapsed = 0, cooldown = 0, lastStart = -1;
    int step = 0;
    std::uint32_t serial = 0;
    float rangeScale = 1;
    bool mantisEffect = false;
};
struct Hero
{
    PresentationPosition presentation;
    bool testInvulnerable=false;
    bool transitionInvincible=false;
    Vec2 renderPosition(PresentationTime time) const { return presentation.sample(position,time); }
    Vec2 position{}, velocity{};
    float platformDisplacement=0; // Consumed inside the next fixed presentation step.
    Contacts contacts{};
    Abilities abilities{};
    SuperDashState superDash;
    SurfaceSwimState surfaceSwim;
    Charms charms{};
    Vitals vitals{};
    Actor actor = Actor::Airborne;
    Input input{};
    NailState nail{};
    std::uint32_t terrainNailSerial = 0;
    std::uint32_t groundJumpSerial=0,groundDashSerial=0,redDamageSerial=0;
    std::uint32_t lightDamageSerial=0; // Presentation event for accepted recoil hits.
    float terrainNailTime = 0;
    Strike terrainNailDirection = Strike::Right;
    int nailDamage = 5; // PlayerData default; upgrades are separate progression data.
    int effectiveNailDamage() const { return charms.fury&&vitals.health==1?(nailDamage*7+2)/4:nailDamage; }
    unsigned shellHits=4,thornsSerial=0;
    float shellCooldown=0;
    int nailHitDamage() const { return effectiveNailDamage(); }
    bool facingRight = true, walkZone = false, jumping = false, doubleJumping = false;
    bool doubleJumpUsed = false, airDashUsed = false, dashing = false, shadeDashing = false, downDash = false;
    bool wallSliding = false, wallLocked = false, bouncing = false, mushroomBouncing = false;
    bool gravityEnabled = true, falling = false, willHardLand = false, controlEnabled = true, inputEnabled = true;
    int wallSide = 0, wallJumpDirection = 0, jumpStep = 0, jumpedSteps = 0, doubleStep = 0;
    int wallLockStep = 0, wallUnstickStep = 0, ledgeSteps = 0, headSteps = 0;
    int jumpQueued = -1, doubleQueued = -1, dashQueued = -1, attackQueued = -1;
    int recoilDirection = 0, recoilStep = 0;
    bool largeRecoil = false;
    float wallSpeed = 0, dashTime = 0, dashCooldown = 0, shadeCooldown = 0;
    float elapsed = 0, fallTime = 0, landingTime = 0, bounceTime = 0;
    float invulnerableTime = 0, damageTime = 0;
    float invulnerableDelay = 0;
    bool invulnerabilityStarted = false;
    Vec2 damageVelocity{};
    // Damage recoil waits for its shared GameClock owner in Simulation.
    // updateUnscaled remains an isolated damage-freeze control for host tests.
    int freezePhase = 0;
    float freezeTime = 0, scale = 1;
    FocusPhase focus = FocusPhase::None;
    float focusTime = 0, drainTime = 0, drainElapsed = 0;
    int drained = 0, focusStartSoul = 0;
    bool refocusing = false;
    // Cast events are consumed by the fixed projectile pool, independent of input edges.
    float castTime = 0, dialogueCastDelay = 0;
    std::uint32_t spellSerial = 0;
    float lookTime = 0;
    bool lookUp = false, lookDown = false;

    void enterSurfaceWater();
    void cancelSurfaceWater();
    bool updateSurfaceWater(float dt);
    void fixedSurfaceWater(float dt, const World& world);
    const char* surfaceWaterClip() const;
    void update(const Input& command, float dt, const World& world = {}); // Unity Update counterpart (scaled seconds).
    void fixedUpdate(float dt, const World& world); // Physics phase, before integration.
    void updateUnscaled(float dt);
    void startInvulnerability();
    void resumeCoroutines(float scaled);
    bool takeDamage(int amount, int sourceSide); // Ordinary enemy damage; side -1 left, +1 right.
    void nailContact(bool bounceable = true, bool large = false, bool mushroom = false);
    void reactToNailContact(Strike direction, bool large = false, bool mushroom = false);
    void checkTerrainThunk(float dt, const World& world);
    bool canJump() const;
    bool canDoubleJump() const;
    bool canDash() const;
    bool canSuperDash() const;
    void startSuperDash();
    void resumeSuperDash(int direction);
    void cancelSuperDash();
    bool updateSuperDash(float dt,const World& world);
    void fixedSuperDash(float dt,const World& world);
    bool canAttack() const;
    bool canFocus() const;
    bool canCast() const;
    void preventCastByDialogueEnd();
    bool startCast();
    bool canWallJump() const;
    bool canWallSlide() const;
    void stopAttack();
    void stopDash();
    void stopJump();
    void startJump();
    void startWallJump();
    void startDoubleJump();
    void startDash();
    void startAttack();
    void updateFocus(float dt);
};
}
