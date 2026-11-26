#pragma once
#include "enemy.hpp"
#include "audio.hpp"
#include <array>

namespace hk3ds {
// Append-only identities for the Crossroads C encounter adapter.
enum class EnemyCKind : unsigned { HuskBully, Baldur, AspidMother, GlowingWomb, BroodingMawlek, Hatchling,
    MushroomBrawler, Sentry, HeavySentry, FlyingSentry, Count };
enum class EnemyCPhase { Idle, Move, Windup, Attack, Recover, Jump, Dead, Gone, Caged };
struct EnemyCProfile {
    const char* prefix;
    int hp;
    unsigned geo;
    Rect body;
    float speed, gravity;
};
const EnemyCProfile& enemyCProfile(EnemyCKind);
struct EnemyCActor {
    EnemyCKind kind=EnemyCKind::HuskBully;
    EnemyCPhase phase=EnemyCPhase::Gone;
    Vec2 position{},velocity{},home{};
    float scaleX=1,timer=0,age=0,flash=0;
    // Placement |scale| (Transform m_LocalScale): colliders, regions and hitboxes scale with it.
    Vec2 size{1,1};
    unsigned attacks=0,injurySerial=0,killSerial=0;
    std::uint32_t audioSerial=0;
    std::uint32_t cageOwner=0;
    bool motherAlerted=false;
    // Ruins Sentry FSM step (EnemyCSentryStep), its clip clock and the alerted latch.
    std::uint8_t step=0;
    bool alerted=false;
    float clipTime=0;
    // Ruins Sentry Fat: Air Attack Pause, jump distance and Can Distance Charge.
    float airPause=0,jumpSpeed=0;
    bool canCharge=true;
    // Ruins Flying Sentry "Flying Sentry Nail": Start Idle (per placement), the battle-wave B
    // regions, IdleBuzz acceleration/wait, FaceDirection turn pause, Lost Hero Check counter,
    // and the Recoil component (translation sweep, speed 15 x magnitude for 0.15 s).
    bool startIdle=false,battleRegions=false,fsmStarted=false,recoilSweeping=false;
    // Shroom Brawler: Facing Right (per placement), Move Time/Speed, Wait Time/Timer.
    bool facingRight=false;
    float moveTime=0,moveSpeed=0,waitTime=0,waitTimer=0;
    Vec2 buzzAcceleration{},recoilDirection{};
    float buzzWait=0,turnPause=0,lostCounter=0,recoilTime=0,recoilMagnitude=1;
    bool motherIdleStarted=false;
    Vec2 motherIdleAcceleration{};
    float motherIdleWait=0;
    EnemySourceDepth audioDepth;
    double audioSeconds=0;
    AnimationPlayer animation;
    PresentationPosition presentation;
    HatchlingSteering hatchling;
    bool alive() const { return phase!=EnemyCPhase::Dead&&visible(); }
    bool visible() const { return phase!=EnemyCPhase::Gone&&phase!=EnemyCPhase::Caged; }
    bool occupied() const { return phase!=EnemyCPhase::Gone; }
    // Body collider in world space: X mirrored and scaled by scaleX, Y scaled by the placement.
    Rect body() const { const Rect b=enemyCProfile(kind).body; return boxBounds({},{b.left,b.bottom*size.y,b.right,b.top*size.y},scaleX); }
    Rect bounds() const { const Rect b=body(); return {position.x+b.left,position.y+b.bottom,position.x+b.right,position.y+b.top}; }
    // Face left (+1) or right (-1), keeping the placement magnitude.
    void face(float direction) { scaleX=(direction<0?-1.f:1.f)*size.x; }
};
// Shot Mawlek NoDrip: gravity 0.6, box 0.64 x 0.56 scaled 1.15-1.45, Idle then Impact on terrain.
struct EnemyCShot {
    bool active=false,impact=false;
    Vec2 position{},velocity{};
    float age=0,scale=1;
    AnimationPlayer animation;
    PresentationPosition presentation;
};
// Brooding Mawlek (Crossroads_09 Mawlek Body): FSMs "Mawlek Control" (body), "Mawlek Head",
// "Mawlek Arm Control" x2 and the Dummy animator that shows the whole body during supers.
struct EnemyCMawlek {
    std::uint8_t body=0,head=0,arm[2]{};
    float bodyClock=0,headClock=0,armClock[2]{},wait=0,headWait=0,cooldown=1,xDistance=0,startX=0;
    // Walker (speed 3, walk/pause 1-3 s, edgeXAdjuster 0.5, no scale change, turn clip Idle Turn).
    std::uint8_t walker=0;
    float walkerClock=0;
    float walkDirection=-1;
    int jumps=0,spits=0;
    bool superRepeated=false,bodyVisible=false,partsAwake=false,armStruck[2]{};
    float dummyScale=1;
    AnimationPlayer head_,arms[2],dummy;
};
// Mushroom Brawler Vomit Glob: stage 0 air, 1 landed puddle, 2 shrink, 3 hidden before recycle.
struct EnemyCGlob {
    bool active=false;
    std::uint8_t stage=0;
    Vec2 position{},velocity{};
    float scale=1,shrinkFrom=1,timer=0;
    AnimationPlayer animation;
    PresentationPosition presentation;
};
struct EnemyCDrop { Vec2 position{}; unsigned count=0;EnemyCKind kind=EnemyCKind::HuskBully;unsigned actor=0; };
class EnemyCEncounter {
    friend struct SimulationStateHash;
public:
    explicit EnemyCEncounter(EnemyRandom random={}) : random_(random) {}
    static constexpr unsigned capacity=24,shotCapacity=40,globCapacity=12;
    bool bind(const AnimationBank*,const NailBank*,EnemyRandom random={});
    void clear();
    // scale: the placement's Transform scale; its X sign is the initial facing (+1 left).
    bool spawn(EnemyCKind,Vec2,float sourceDepth=0,Vec2 scale={1,1});
    void fixedUpdate(float dt,Hero&,const World&);
    void update(float dt,const Hero&,const World&);
    bool active() const;
    std::array<EnemyCActor,capacity> actors{};
    std::array<EnemyCShot,shotCapacity> shots{};
    std::array<EnemyCGlob,globCapacity> globs{};
    EnemyCMawlek mawlek;
    std::array<CombatObject,capacity> objects{};
    std::array<EnemyCDrop,capacity> drops{};
    unsigned dropCount=0,rejectedSpawns=0;
    unsigned injurySerial=0,killSerial=0;
    const AnimationBank* bank() const { return bank_; }
private:
    const AnimationBank* bank_=nullptr;
    const NailBank* nails_=nullptr;
    EnemyRandom random_{};
    // Deterministic fallback for fixtures without an injected RNG.
    std::uint32_t randomState_=0x9E3779B9u;
    float roll(float low,float high);
    std::uint32_t nextAudioSerial_=0;
    // Hero position of the last fixed update: TOOK DAMAGE turns the Husk Bully toward it.
    Vec2 hero_{};
    CombatScene combat_;
    static void receive(void*,const NailHit&,HitOutcome);
    struct Receiver { EnemyCEncounter* encounter=nullptr; unsigned index=0; };
    std::array<Receiver,capacity> receivers_{};
    void hit(unsigned,const NailHit&,HitOutcome);
    void enter(unsigned,EnemyCPhase,float,const char*);
    void play(unsigned,const char*);
    unsigned spawnGlob(Vec2,Vec2);
    void updateGlobs(float,Hero&,const World&);
    void shoot(Vec2,Vec2);
    void motherIdle(EnemyCActor&,float dt);
    void returnToCage(unsigned);
    bool releaseFromCage(Vec2);
};
struct EnemyCRenderActor {
    AnimationPlayer animation;
    Vec2 position{};
    float scaleX=1,alpha=1,flash=0;
    bool visible=false;
    // Placement Y scale (X magnitude is in scaleX).
    float scaleY=1;
};
struct EnemyCRenderState {
    std::array<EnemyCRenderActor,EnemyCEncounter::capacity> actors{};
    struct Shot { AnimationPlayer animation; Vec2 position{}; float scale=1; };
    std::array<Shot,EnemyCEncounter::shotCapacity> shots{};
    unsigned shotCount=0;
    // Brooding Mawlek parts: arms and head behind the body, the Dummy in front.
    struct Part { AnimationPlayer animation; Vec2 position{}; float scaleX=1,scaleY=1; bool front=false; };
    std::array<Part,4> parts{};
    unsigned partCount=0;
    unsigned mawlekActor=EnemyCEncounter::capacity;
    struct Glob { AnimationPlayer animation; Vec2 position{}; float scale=1; };
    std::array<Glob,EnemyCEncounter::globCapacity> globs{};
    unsigned globCount=0;
    void capture(const EnemyCEncounter&,PresentationTime);
};
}
