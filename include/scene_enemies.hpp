#pragma once
#include "scene.hpp"
#include "crossroads_enemy.hpp"
#include "enemy_b.hpp"
#include "enemy_c.hpp"
#include "crossroads_boss.hpp"
#include "watcher_knights.hpp"
#include "mantis_lords.hpp"
#include "broken_vessel.hpp"
#include "enemy_rewards.hpp"
#include "elder_baldur.hpp"

namespace hk3ds {
enum class SceneEnemyKind { Unsupported, Crawler, Tiktik, Gruzzer, Vengefly, Hatchling,
    Wandering, Hornhead, Leaping, Goam, Bully, Baldur, Mother, Mawlek,
    Brawler, Sentry, HeavySentry, FlyingSentry, ElderBaldur, ZombieGuard, Spitter, FungusFlyer, Jellyfish, JellyfishBaby, JellyfishInert, FungoonBaby, FungCrawler, GruzMother, FalseKnight, DungDefender };
SceneEnemyKind sceneEnemyKind(const char* prefab);
struct SceneEnemyMovementAudio {
    std::uint32_t serial=0;
    EnemyClip clip=EnemyClip::Count;
    Vec2 position{};
    float depth=0;
    float pitch=1;
    // AudioSource.Play restarts within one activation (e.g. Walker.EndTurning).
    std::uint32_t playSerial=0;
    EnemyLoopProfile profile=EnemyLoopProfile::Default;
};
struct SceneEnemyDraw {
    AnimationPlayer animation;
    Vec2 position{};
    float scaleX=1,scaleY=1,rotation=0,depth=.004f;
    unsigned bank=0;
    bool foreground=false;
    unsigned owner=0;
};
struct SceneEnemyRenderState {
    static constexpr unsigned capacity=128;
    std::array<SceneEnemyDraw,capacity> actors{};
    unsigned count=0;
    bool locked=false,watcherBattle=false;
    float mantisTitleAlpha=0;
    float mantisGateOffset=0;
    AnimationPlayer mantisHeroAnimation;
    Rect arena{};
    EnemyCRenderState c;
    std::array<unsigned,EnemyCEncounter::capacity> cOwners{};
};
// One resident room. Banks and actor storage change only at room entry.
class SceneEnemies {
    friend struct SimulationStateHash;
public:
    static constexpr unsigned capacity=64,bankCount=15;
    static constexpr unsigned movementCapacity=capacity+EnemyCEncounter::capacity;
    bool bind(const SceneBank&,const char* root,const NailBank&,const AnimationBank& crawlerArt,
              const CrawlerConfig&,EnemyRandom,const World&,std::uint32_t flags,CrossroadsBoss&,
              WatcherProgress={},unsigned nailSmithUpgrades=0,const char* sceneName="",EnemyLifeState* life=nullptr,const SceneActivationState* activation=nullptr,SceneBoolSave* sceneBools=nullptr,bool mantisDefeated=false,BrokenVesselProgress brokenProgress={},bool cityCrestSpent=false);
    bool restart(const World&,std::uint32_t flags);
    bool updateActivation(const SceneActivationState&,const World&);
    void clear();
    void fixedUpdate(float,Hero&,const World&,bool frozen=false);
    void update(float,Hero&,const World&,bool frozen=false,Vec2 camera={});
    void capture(SceneEnemyRenderState&,PresentationTime)const;
    const AnimationBank& bank(unsigned i)const{return i==0?*crawlerArt_:banks_[i-1];}
    bool usesBank(unsigned i)const{return used_[i];}
    const char* encounterArtStem()const{return mantisRoom_?"mantis":brokenRoom_?"broken-vessel":"watcher";}
    unsigned count()const{return count_;}
    // Native play-on-awake sources; FSMActivator only disables FSMs, not audio.
    SceneEnemyMovementAudio movementAudio(unsigned slot)const;
    CombatScene& combat(){return combat_;}
    const CombatScene& combat()const{return combat_;}
    unsigned unsupported=0;
    unsigned takeGeo(){auto n=geo_;geo_=0;return n;}
    void releaseRewards(GeoBank& bank){rewards_.release(bank);}
    void setGreed(bool enabled){rewards_.setGreed(enabled);}
    bool takeFreeze(){auto f=freeze_;freeze_=false;return f;}
    std::uint32_t savedFlags()const{return flags_;}
    const SceneSpawn* bossSpawn()const;
    Rect arenaFor(const SceneSpawn&,const World&)const;
    World world(const World&,const CrossroadsBoss&);
    bool hides(unsigned owner,const CrossroadsBoss&)const;
    bool locked()const{return mantis.locked()||brokenVessel.locked()||watchers.locked()||(mawlekStarted_&&c_.actors[mawlek_].alive());}
    WatcherEncounter watchers;
    MantisLords mantis;
    BrokenVesselEncounter brokenVessel;
    const char* roomArtStem()const{return encounterArtStem();}
    void restoreBrokenVesselProgress(BrokenVesselProgress p){brokenProgress_=p;}
    SceneBattle battle;
    void restoreWatcherProgress(WatcherProgress p){watcherProgress_=p;}
private:
    struct Slot {SceneEnemyKind kind=SceneEnemyKind::Unsupported;unsigned actor=0,bank=0,spawn=0;float scaleY=1;bool active=true;std::uint32_t owner=0;EnemyRewardConfig reward{};float goamRotation=0;std::uint32_t cSerial=0;};
    EnemyRewardQueue<capacity> rewards_;
    EnemyLifeState* life_=nullptr;
    char scene_[32]{};
    void killed(const Slot&,Vec2,float,float rotation=0);
    std::vector<SceneSpawn> spawns_;
    std::vector<unsigned> activationSpawns_;
    SceneBossRoom bossRoom_;
    std::array<Slot,capacity> slots_{};
    std::array<EnemySourceDepth,capacity> audioDepths_{};
    std::array<std::uint32_t,capacity> audioSerials_{};
    std::uint32_t nextAudioSerial_=0;
    double audioSeconds_=0;
    void activateMovementAudio(unsigned slot);
    std::array<CrossroadsEnemy,32> a_{};
    std::array<EnemyB,24> b_{};
    std::array<Crawler,16> crawlers_{};
    // Save-driven spawns can grow count_; encounter objects keep fixed slots.
    static constexpr unsigned watcherObjectBegin=capacity+CrossroadsBoss::objectCapacity;
    std::array<CombatObject,watcherObjectBegin+WatcherEncounter::capacity> objects_{};
    std::array<AnimationBank,bankCount-1> banks_;
    std::array<bool,bankCount> used_{};
    std::array<CrossroadsEnemyConfig,4> aConfig_{};
    EnemyBConfigs bConfig_{};
    CrawlerConfig crawlerConfig_{};
    const AnimationBank* crawlerArt_=nullptr;
    const NailBank* nails_=nullptr;
    EnemyRandom random_{};
    CrossroadsBoss* boss_=nullptr;
    EnemyCEncounter c_;
    ElderBaldur elder_;
    struct ZombieGuard {
        enum class Phase { Dormant, Wake, Idle, Turn, Walk, Run, Stop, Return, Anticipate,
            Strike, Recoil, StrikeEnd, StompAntic, StompJump, Land, Cooldown, Dead, Startle };
        Phase phase=Phase::Dead;
        Vec2 position{},velocity{},home{};
        PresentationPosition presentation;
        AnimationPlayer animation;
        AnimationPlayer swipe;
        struct Spurt {Vec2 position{};AnimationPlayer animation;float age=0;};
        // Shockwave Wave (sharedassets48:63): Speed 18 -> 0.45 at start, +36 per second.
        struct Wave {Vec2 position{};bool active=false;float speed=0,endTimer=0;};
        std::array<Wave,2> waves{};
        std::array<std::array<Spurt,20>,2> spurts{};
        float scaleX=-1,timer=0;
        unsigned clubRepeats=0,stompRepeats=0;
        // Woken (cleared by Return Left/Right; the next alert plays Startle).
        bool woken=true;
        bool bind(CombatObject&,const AnimationBank&,Vec2,float);
        void step(CombatObject&,const AnimationBank&,EnemyRandom,float,Hero&,const World&);
        void enter(Phase,const AnimationBank&,const char*,float=0);
    } guard_;
    struct Spitter {
        enum class Phase { Idle, Chase, Back, Fire, Dead };
        struct Shot {
            Vec2 position{},velocity{};
            PresentationPosition presentation;
            AnimationPlayer animation;
            float age=0;
            bool impact=false;
        };
        std::array<Shot,12> shots{};
        Phase phase=Phase::Idle;
        Vec2 position{},home{},velocity{},idleAcceleration{};
        PresentationPosition presentation;
        AnimationPlayer animation;
        float timer=0,scaleX=1,shotAngle=0,idleWait=0,rangeOutTimer=0;
        bool fired=false,spread=false;
        bool bind(CombatObject&,const AnimationBank&,Vec2,float);
        void step(CombatObject&,const AnimationBank&,EnemyRandom,float,Hero&,const World&);
        void shoot(const AnimationBank&,float,float);
    };
    std::array<Spitter,4> spitters_{};
    struct FungusFlyer {
        enum class Phase { Sleep, Idle, Antic, Spit, Recover, Cooldown, Dead };
        struct Gas {
            Vec2 position{},velocity{};
            PresentationPosition presentation;
            float age=0;
            bool active=false;
        };
        std::array<Gas,23> gas{};
        Phase phase=Phase::Sleep;
        Vec2 position{},home{},velocity{},target{};
        PresentationPosition presentation;
        AnimationPlayer animation,effect;
        float timer=0,scaleX=1,nextGas=0;
        bool bind(CombatObject&,const AnimationBank&,Vec2,float);
        void step(CombatObject&,const AnimationBank&,EnemyRandom,float,Hero&,const World&);
    };
    std::array<FungusFlyer,4> flyers_{};
    struct Jellyfish {
        enum class Phase { Sleep, Idle, Jump, Corpse, Startle, Chase, Dead };
        Phase phase=Phase::Sleep;
        SceneEnemyKind kind=SceneEnemyKind::Unsupported;
        Vec2 position{},home{},velocity{},direction{};
        PresentationPosition presentation;
        AnimationPlayer animation;
        float timer=0,scaleX=1;
        bool bind(CombatObject&,const AnimationBank&,SceneEnemyKind,Vec2,float,EnemyRandom);
        Rect body()const;
        void die(CombatObject&,const AnimationBank&,EnemyRandom);
        void step(CombatObject&,const AnimationBank&,EnemyRandom,float,Hero&,const World&);
    };
    std::array<Jellyfish,capacity> jellyfish_{};
    struct FungoonBaby {
        enum class Phase { Sleep, Awake, Dead };
        Phase phase=Phase::Sleep;
        Vec2 home{},position{},velocity{},acceleration{};
        // FaceDirection pauseBetweenTurns 0.5 s.
        float turnPause=0;
        PresentationPosition presentation;
        AnimationPlayer animation;
        float timer=0,scaleX=1;
        bool bind(CombatObject&,const AnimationBank&,Vec2,float);
        Rect body()const;
        void step(CombatObject&,const AnimationBank&,EnemyRandom,float,Hero&,const World&);
    };
    std::array<FungoonBaby,capacity> fungoonBabies_{};
    SceneBoolSave* sceneBools_=nullptr;
    bool elderPlacement_=false;
    bool elderDefeated()const;
    CombatScene combat_;
    std::vector<Solid> solids_;
    std::vector<TerrainShape> lockShapes_;
    std::vector<Vec2> lockVertices_;
    Rect mawlekArena_{};
    unsigned count_=0,geo_=0,mawlek_=EnemyCEncounter::capacity;
    bool cityCrestSpent_=false;
    std::uint32_t flags_=0;
    bool freeze_=false,mawlekStarted_=false,watcherRoom_=false,mantisRoom_=false,mantisDefeated_=false,brokenRoom_=false;
    WatcherProgress watcherProgress_{};
    BrokenVesselProgress brokenProgress_{};
    unsigned nailSmithUpgrades_=0;
    bool activateSpawn(unsigned,const World&);
    unsigned combatObjectCount() const;
};
}
