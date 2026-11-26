#pragma once
#include "uumuu_attack_data.hpp"
#include "uumuu_death.hpp"
#include "uumuu_multizaps.hpp"
#include "uumuu_zap.hpp"
#include "uumuu_progress.hpp"
#include "uumuu_body.hpp"
#include "uumuu_colliders.hpp"
#include "uumuu_wake.hpp"
#include "uumuu_gates.hpp"
#include <limits>
#include <string_view>

namespace hk3ds {
// Captured synchronously for each accepted hit. NaN means the attack's source
// magnitude was not supplied; it is never replaced by a gameplay default.
struct UumuuRecoilFrame {
    Rect collider{};
    float magnitude=std::numeric_limits<float>::quiet_NaN();
    Vec2* body=nullptr;
    Vec2* transform=nullptr;
    const World* world=nullptr;
};
struct UumuuHitAdapter {
    void* context=nullptr;
    bool (*capture)(void*,const NailHit&,const NailContact*,UumuuRecoilFrame&)=nullptr;
    // Before the initial sweep, deliver RECOIL HORIZONTAL (horizontal only),
    // then HIT <direction> synchronously.
    void (*recoilStarted)(void*,Strike)=nullptr;
};
struct UumuuSlashPose {
    float localRotationZ=0,scaleX=1;
};
enum class UumuuArrivalPhase { Inactive,FirstPause,Arrive,Leave,SlashPause,AttackPause,Ready,Slashed };
struct UumuuArrivalEvents {
    // arrive: Check Pos audio at Quirrel Land's OLD pose, then place/scale
    // Quirrel Land, enable its mesh, play Q Arrive audio at Quirrel Slash,
    // and play Quirrel Arrive on Quirrel Land. leave plays Q Leave audio at
    // Quirrel Slash, then Quirrel Leave on Quirrel Land.
    bool arrive=false,leave=false,hide=false;
    // At Quirrel Slash: play the two source audio actions, sample rotation
    // (float Range) then scale (integer Range), and call prepareSlashAntic.
    // Apply its pose, enable the mesh and play Slash Antic. Only that
    // animation's FINISHED event calls beginSlash.
    bool slashAntic=false;
    Vec2 position{};
    float scaleX=1;
};
// Room-owned composition of the source controllers. Each named callback is
// separate: the room must supply the actual animation, physics and coroutine
// schedule. This does not infer Quirrel navigation or corpse spawning.
class UumuuEncounter {
    friend struct SimulationStateHash;
public:
    UumuuEncounter()=default;
    UumuuEncounter(const UumuuEncounter&)=delete;
    UumuuEncounter& operator=(const UumuuEncounter&)=delete;
    ~UumuuEncounter(){reset();}
    // Native storage for a whole authored burst, not a source pool-size claim.
    static constexpr unsigned zapCapacity=uumuu_attack_data::zapCount;
    bool bind(CombatObject&,Vec2 position,EnemyRandom,bool defeatedMegaJelly);
    // Production admission seam: the regular archive conditionally loads its
    // boss overlay. The room owner supplies the typed saved source bool and
    // resets on exit. Refused calls preserve an existing binding.
    static bool archiveOverlayRequired(std::string_view sceneName,bool defeatedMegaJelly);
    bool bindArchiveRoom(CombatObject&,std::string_view sceneName,EnemyRandom,bool defeatedMegaJelly);
    // Borrows the actual save-owner state. It must outlive this binding.
    // Saving defeat follows real corpse activation, independently of gates.
    bool bindArchiveRoom(CombatObject&,std::string_view sceneName,EnemyRandom,UumuuProgress&);
    // The room captures its live collider/poses/world and the actual attack's
    // serialized magnitude. No pose is retained between hits.
    bool bindHitAdapter(UumuuHitAdapter);
    // Optional room-owned presenter. Source FSM entries restart its clips;
    // the room advances animation separately from the control callback.
    bool bindBody(UumuuBodyActor&);
    // Bind the room's initialized overlay gates before arenaEntered. Their
    // storage outlives this binding; reset only releases the borrowed pointer.
    // Battle callbacks dispatch immediately, independently of event draining.
    // Saved defeat omits the overlay; a completed battle cannot borrow gates.
    bool bindGates(UumuuGates&);
    // Called for the source actor's room-owned health slot. Geometry follows
    // the live motion pose; collider enablement follows the wake Burst entry.
    bool bindBodyColliders();
    bool syncBodyColliders(); // Also call after native physics/recoil callbacks.
    void contactBody(Hero& hero,Rect body)const{colliders_.contactHero(hero,body);}
    void reset();
    bool arenaEntered();
    // Scaled iTween callback, separate from the control FSM Update.
    bool wakeTweenUpdate(float scaledDt);
    bool arrivePause(Vec2 heroPosition); // Source Arrive Pause: true selects ATTACK.
    bool quirrelLeaveAnimationFinished(Vec2 heroPosition);
    bool slashAntic(Vec2 heroPosition);
    // Supplies real room RNG results: rotation in the authored inclusive float
    // range; scaleRoll from Unity Random.Range(0,100), maximum exclusive.
    // A rejected call leaves both the output and current state unchanged.
    bool prepareSlashAntic(float rotationSample,int scaleRoll,UumuuSlashPose&);
    bool beginSlash(Vec2 heroPosition);
    bool slashAnimationFinished(Vec2 heroPosition);
    bool afterQuirrelRecovery(Vec2 heroPosition);
    // Drain outputs after each callback, before advancing that source again.
    void controlUpdate(float dt,Vec2 heroPosition);
    void lightningUpdate(float dt);
    void contactLightning(Hero&,Rect worldHeroBox)const;
    void battleUpdate(float dt);
    // Only the real corpse's activation calls this; HP loss never substitutes.
    bool corpseActivated(bool sourceBossScene);
    void corpseUpdate(float dt);
    // HealthManager -> EnemyDeathEffects emits external3/path32 on lethal hit.
    // The room activates that real prefab, then calls corpseActivated.
    bool takeCorpseSpawnRequest(){return shell_.takeLethalHit();}
    std::uint32_t takeBounceShroomDestroyRequest(){return shell_.takeBounceShroomDestroyRequest();}
    bool bounceShroomDestroyed(std::uint32_t token){return shell_.bounceShroomDestroyed(token);}
    bool locked()const{return battle_.locked();}
    bool completed()const{return battle_.completed();}
    bool poolExhausted()const{return poolExhausted_;}
    unsigned activeZaps()const;
    UumuuShellPhase shellPhase()const{return shell_.phase();}
    UumuuAttackPhase attackPhase()const{return attacks_.phase();}
    UumuuArrivalPhase arrivalPhase()const{return arrivalPhase_;}
    UumuuArrivalEvents takeArrivalEvents(){auto e=arrivalEvents_;arrivalEvents_={};return e;}
    UumuuWakeEvents takeWakeEvents(){return wake_.takeEvents();}
    const UumuuWake& wake()const{return wake_;}
    UumuuAttackEvents takeAttackEvents(){auto e=attackEvents_;attackEvents_={};return e;}
    UumuuBattleEvents takeBattleEvents(){auto e=battleEvents_;battleEvents_={};return e;}
    UumuuQuirrelEvents takeQuirrelEvents(){return quirrel_.takeEvents();}
    UumuuDeathPhase deathPhase()const{return death_.phase();}
    UumuuDeathEvents takeDeathEvents(){auto e=deathEvents_;deathEvents_={};return e;}
    const UumuuZap* zap(unsigned i)const{return i<zapCapacity?&zaps_[i]:nullptr;}
    UumuuZapEvents takeZapEvents(unsigned i){return i<zapCapacity?zaps_[i].takeEvents():UumuuZapEvents{};}
    UumuuMultizaps& multizaps(){return multizaps_;}
    const UumuuMultizaps& multizaps()const{return multizaps_;}
    // Native room collision, bounce, bounds and pose callbacks stay explicit.
    UumuuMotion& motion(){return motion_;}
    const UumuuMotion& motion()const{return motion_;}
    UumuuRecoil& recoil(){return recoil_;}
private:
    UumuuBodyActor* body_=nullptr;
    UumuuGates* gates_=nullptr;
    static void acceptedHit(void*,const NailHit&,const NailContact*);
    void dispatchAttack(Vec2 heroPosition);
    void arrivalUpdate(float dt,Vec2 heroPosition);
    void dispatchDeath();
    void dispatchBattle();
    CombatObject* object_=nullptr; // Borrowed slot outlives the encounter.
    UumuuProgress* progress_=nullptr;
    UumuuHitAdapter hitAdapter_{};
    EnemyRandom random_{};
    UumuuShell shell_;
    UumuuColliders colliders_;
    UumuuBattle battle_;
    UumuuBattleEvents battleEvents_{};
    UumuuWake wake_;
    UumuuQuirrel quirrel_;
    UumuuAttacks attacks_;
    UumuuMotion motion_;
    UumuuRecoil recoil_;
    UumuuMultizaps multizaps_;
    std::array<UumuuZap,zapCapacity> zaps_{};
    UumuuDeath death_;
    UumuuAttackEvents attackEvents_{};
    UumuuDeathEvents deathEvents_{};
    bool poolExhausted_=false;
    UumuuArrivalPhase arrivalPhase_=UumuuArrivalPhase::Inactive;
    UumuuArrivalEvents arrivalEvents_{};
    float arrivalTimer_=0;
    bool arrivalReceived_=false,slashAnticReceived_=false,slashPoseReceived_=false;
};
}
