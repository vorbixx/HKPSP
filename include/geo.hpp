#pragma once
#include "enemy.hpp"
namespace hk3ds {
enum class JournalEnemy : unsigned { Crawler, Buzzer };
struct GeoEnemyConfig { Vec2 effectOrigin{}; char journalKey[48]{}; };
struct GeoConfig {
    Rect body{};
    PhysicsMaterial material{};
    float gravity=0, pickupDelay=0, speedMin=0, speedMax=0, angleMin=0, angleMax=0;
    float bounceFactor=0,bounceThreshold=0,bounceRandomMin=0,bounceRandomMax=0;
    float depthMin=0,depthMax=0;
    unsigned value=0;
    std::array<GeoEnemyConfig,2> enemies{};
    bool load(const char*);
};
struct JournalProgress { bool killed=false,newData=false; unsigned remaining=0; };
struct JournalEvent {
    JournalEnemy enemy{}; const char* key=nullptr;
    bool firstKill=false,notesCompleted=false,showMessage=false;
};
// initial remaining count and hasJournal are supplied by save data, never guessed.
JournalEvent recordJournalKill(JournalEnemy,const GeoConfig&,JournalProgress&,bool hasJournal);
struct GeoCoin {
    bool active=false,idle=false,touchingHero=false;
    Vec2 position{},velocity{};
    Vec2 renderPosition(PresentationTime time) const { return presentation.sample(position,time); }
    PresentationPosition presentation;
    float age=0;
    // HealthManager emits three distinct denominations (1, 5, 25).
    unsigned value=0,size=0;
    // Presentation observations only; never consumed by physics or gameplay RNG.
    float depth=0;
    unsigned spawnGeneration=0,collisionGeneration=0;
    unsigned sampleStep=0;
    Vec2 lastSamplePosition{},sampleDisplacement{};
    float sampleSpeed=0;
    std::array<unsigned,16> contactIds{};
    unsigned contactCount=0;
};
class GeoBank {
    friend struct SimulationStateHash;
public:
    // Native bounded room capacity. Overflow rejects the whole drop, never truncates.
    static constexpr unsigned capacity=256;
    bool bind(const GeoConfig&,EnemyRandom);
    bool spawn(unsigned count,JournalEnemy,Vec2 enemyPosition,float scaleX=1);
    // Position already includes HealthManager.effectOrigin; no journal offset.
    bool spawnReward(unsigned small,unsigned medium,unsigned large,Vec2 position,bool megaFling=false);
    void update(float scaledDt);
    void fixedUpdate(float dt,const World&);
    // Returns newly collected value. HeroBox trigger Enter is required after delay;
    // overlapping before the delay does not turn into a synthetic Stay pickup.
    unsigned contactHero(Rect heroBox);
    void gather(Vec2 hero,float dt);
    Rect bounds(unsigned index) const;
    const std::array<GeoCoin,capacity>& coins() const { return coins_; }
    unsigned activeCount() const;
    unsigned pickups=0,bounces=0,collectedValue=0;
private:
    GeoConfig config_{}; EnemyRandom random_{};
    std::array<GeoCoin,capacity> coins_{};
    bool ready_=false;
    bool spawnCoins(unsigned count,Vec2 position,unsigned size,unsigned value,bool megaFling=false);
};
}
