#pragma once
#include "save.hpp"
#include "enemy.hpp"
#include "enemy_rewards.hpp"
#include "scene.hpp"
#include <array>

namespace hk3ds {
// Serialized identities are append-only.
enum class GreenpathKind : unsigned { Mosscreep, Mossfly, MossKnight, Squit, Aluba,
    Durandoo, VolatileMosskin, MossCharger, Hornet, Count };
// Level133 Hornet Throw: Self Pos + (0,-0.5), Needle SetPosition(Self Pos), SetVelocityAsAngle(Angle, Throw Speed 38)
// with Angle = GetAngleToTarget2D(Hero Obj) from the Hornet pivot. Needle PolygonCollider2D local points (x,y),
// tip at (-1.817,0); the needle rotates so its tip points along the velocity (angle + 180 degrees).
inline constexpr float greenpathNeedleDropY=-0.5f;
inline constexpr float greenpathNeedleSpeed=38.f;
inline constexpr float greenpathNeedlePoints[3][2]={{0.6597900f,0.0988083f},{0.6529942f,-0.1331329f},{-1.8165207f,-0.0017796f}};
// Hornet's source FSM has a short dormant trigger and a post-death escape.
// Keep the original enemy states first for stable diagnostics, then append the
// encounter-only states so old replay phase values remain readable. Counter is
// a reserved legacy value: Protector's source FSM never enters that state.
enum class GreenpathPhase { Idle, Move, Antic, Attack, Recover, Jump, Throw, Sphere, Dead, Gone,
    Dormant, Intro, Evade, Counter, Escape, JumpAntic, AirAntic, SphereAntic, EvadeAntic, Stagger, Turn,
    // Mozzie (Squit): Attack Pause, Stop and the wall Pull Out.
    Pause, Stop, PullOut,
    // Moss Knight Control: shield stance, block bump, unshield and the evade's end.
    Shield, Block, Unshield, EvadeEnd };
struct GreenpathRegistration { const char* prefab; GreenpathKind kind; };
inline constexpr GreenpathRegistration greenpathEnemyRegistry[]={
    {"Moss Walker",GreenpathKind::Mosscreep}, {"Pigeon",GreenpathKind::Mossfly},
    {"Moss Knight",GreenpathKind::MossKnight}, {"Mosquito",GreenpathKind::Squit},
    {"Lazy Flyer Enemy",GreenpathKind::Aluba}, {"Acid Walker",GreenpathKind::Durandoo},
    {"Mossman_Shaker",GreenpathKind::VolatileMosskin}, {"Moss Charger",GreenpathKind::MossCharger},
    {"Hornet Boss 1",GreenpathKind::Hornet},
    {"Lazy Flyer",GreenpathKind::Aluba}
};
const GreenpathRegistration* findGreenpathEnemy(const char* prefab);
struct GreenpathProfile {
    const char* prefix;
    int hp;
    unsigned damage,geo;
    Rect body;
    float speed,gravity;
    bool invincible;
    int invincibleDirection;
};
const GreenpathProfile& greenpathProfile(GreenpathKind);
// Presentation uses the converted source convention independently of collision.
float greenpathArtScale(GreenpathKind kind,float facing);
struct GreenpathActor {
    GreenpathKind kind=GreenpathKind::Mosscreep;
    GreenpathPhase phase=GreenpathPhase::Gone;
    Vec2 position{},velocity{},home{};
    PresentationPosition presentation;
    AnimationPlayer animation;
    float facing=1,timer=0,age=0,flash=0;
    // Logical clip key last requested by the state machine (host checks; no bank needed).
    const char* clip=nullptr;
    unsigned attacks=0,hits=0;
    // IdleBuzz acceleration/wait and the Mozzie out-of-sight clock.
    Vec2 buzzAcceleration{};
    float buzzWait=0,sightTimer=0;
    // Moss Knight Control: Lunge1 Speed, state clip clock, Attack Pause, Walker turn cooldown,
    // Shot Repeats, and the Slash Animation / After Evade / shot-fired flags.
    float lungeSpeed=0,stateClock=0,attackPause=0,turnCooldown=0;
    // Maskfly FaceAngle (radians) and Lazy Flyer Flight Distance (per placement); FSM start latch.
    float rotation=0,flightDistance=10;
    bool fsmStarted=false;
    int shotRepeats=0;
    bool quickSlash=false,slashAfterEvade=false,shot=false;
    bool alive()const{return phase!=GreenpathPhase::Dead&&phase!=GreenpathPhase::Gone;}
    bool visible()const{return phase!=GreenpathPhase::Gone;}
    // Placement |scale| (Transform chain): body and authored ranges scale with it.
    Vec2 size{1,1};
    Rect body()const{const Rect b=greenpathProfile(kind).body;return {b.left*size.x,b.bottom*size.y,b.right*size.x,b.top*size.y};}
    Rect bounds()const{return boxBounds(position,body(),-facing);}
};
// Moss Knight Grass Ball (sharedassets149 prefab 34): trigger circle .4, gravity 1, SpinSelf.
struct GreenpathBall {
    bool active=false,broken=false;
    Vec2 position{},velocity{};
    float age=0,rotation=0,spin=0;
    PresentationPosition presentation;
};
struct GreenpathNeedle {
    bool active=false;
    bool returning=false,hitHero=false;
    Vec2 position{},origin{},velocity{},returnStart{};
    float age=0,returnAge=0,angle=0;
    PresentationPosition presentation;
};
class GreenpathEncounter {
    friend struct SimulationStateHash;
public:
    static constexpr unsigned capacity=24;
    // Existing save format admits boss flags through the normal save producer.
    static constexpr std::uint32_t hornetDefeatedFlag=TutorialSave::hornetFlag;
    bool bind(const AnimationBank*,const NailBank*);
    void setRandom(EnemyRandom random){random_=random;}
    void clear(std::uint32_t flags=0);
    // facing: the placement's X scale (sign faces, magnitude sizes); scaleY 0 means |facing|.
    bool spawn(GreenpathKind,Vec2,float facing=1,float scaleY=0);
    bool loadScene(const char* path,std::uint32_t flags=0);
    bool restartScene(std::uint32_t flags=0);
    void setLifecycle(EnemyLifeState*,const char* scene,const std::vector<SceneSpawn>&);
    std::uint32_t owner(unsigned slot)const{return owners_[slot];}
    void releaseRewards(GeoBank& bank){rewards_.release(bank);}
    void setGreed(bool enabled){rewards_.setGreed(enabled);}
    bool beginHornet(Rect arena,std::uint32_t flags=0);
    World arenaWorld()const;
    bool fixture=false;
    void fixedUpdate(float dt,Hero&,const World&);
    void update(float dt,const Hero&);
    bool locked()const;
    CombatScene& combat(){return combat_;}
    const CombatScene& combat()const{return combat_;}
    void removeSavedFlags(std::uint32_t mask){flags_&=~mask;}
    std::uint32_t savedFlags()const{return flags_;}
    std::array<GreenpathActor,capacity> actors{};
    std::array<CombatObject,capacity> objects{};
    GreenpathNeedle needle;
    std::array<GreenpathBall,6> balls{};
    unsigned geoReleased=0,rejectedSpawns=0;
    Rect arena{};
    // Encounter presentation is sampled by the render slot and never drives
    // collision or save state.
    float titleAlpha() const;
    unsigned introPage() const{return introPage_;}
    const char* introText(unsigned page) const;
    bool introActive() const;
    void advanceIntro(Hero&);
    bool phaseTwo() const{return phaseTwo_;}
    unsigned phaseSerial() const{return phaseSerial_;}
    unsigned staggerCount() const{return staggerCount_;}
    bool sphereVisible() const{return sphereVisible_;}
    Vec2 spherePosition() const{return spherePosition_;}
    float sphereRadius() const{return sphereRadius_;}
    float sphereAge() const{return sphereAge_;}
private:
    const AnimationBank* bank_=nullptr;
    const NailBank* nails_=nullptr;
    CombatScene combat_;
    struct Receiver {GreenpathEncounter* owner=nullptr;unsigned index=0;};
    std::array<Receiver,capacity> receivers_{};
    std::uint32_t flags_=0;
    bool hornetArena_=false;
    bool phaseTwo_=false,sphereVisible_=false;
    float titleAge_=-1,sphereAge_=0,sphereRadius_=0;
    Vec2 spherePosition_{};
    Rect trigger_{};
    Vec2 introPosition_{};
    bool authoredIntro_=false;
    unsigned phaseSerial_=0,staggerCount_=0,introPage_=0;
    EnemyRandom random_{};
    std::uint32_t randomState_=0x48524e54;
    std::array<unsigned,4> choiceRepeats_{},choiceMissed_{};
    unsigned stunHits_=0,comboHits_=0;
    float comboAge_=0;
    EnemyRewardQueue<capacity> rewards_;
    EnemyLifeState* life_=nullptr;
    char scene_[32]{};
    char roomPath_[256]{};
    struct Identity {GreenpathKind kind{};Vec2 position{};std::uint32_t owner=0;EnemyRewardConfig reward{};Vec2 scale{1,1};};
    std::array<Identity,capacity> identities_{};
    unsigned identityCount_=0;
    std::array<std::uint32_t,capacity> owners_{};
    std::array<EnemyRewardConfig,capacity> actorRewards_{};
    mutable std::array<Solid,4> arenaSolids_{};
    static void receive(void*,const NailHit&,HitOutcome);
    void hit(unsigned,const NailHit&,HitOutcome);
    void enter(unsigned,GreenpathPhase,float,const char*,bool hold=false);
    void beginIntro(unsigned);
    float random(float low,float high);
    unsigned chooseAttack(bool canThrow);
    void spawnBall(Vec2 position,Vec2 velocity);
    void updateBalls(float dt,Hero&,const World&);
    void play(unsigned,const char*,bool hold=false);
};
struct GreenpathRenderActor {
    AnimationPlayer animation;
    Vec2 position{};
    // facing: signed art X scale including the placement |scale|; scaleY the placement Y scale.
    float facing=1,alpha=1,scaleY=1;
    bool visible=false;
    float rotation=0;
};
struct GreenpathRenderState {
    std::array<GreenpathRenderActor,GreenpathEncounter::capacity> actors{};
    Vec2 needle{};
    float needleAge=0;
    bool needleVisible=false,fixture=false;
    Rect arena{};
    float titleAlpha=0;
    unsigned introPage=0,phaseSerial=0;
    bool introVisible=false;
    const char* introText="";
    Vec2 spherePosition{};
    float sphereRadius=0;
    float sphereAge=0;
    bool sphereVisible=false;
    // Grass Balls in flight (a broken ball hides its SpriteRenderer).
    struct Ball { Vec2 position{}; float rotation=0,scale=1; };
    std::array<Ball,6> balls{};
    unsigned ballCount=0;
    void capture(const GreenpathEncounter&,PresentationTime);
};
}
