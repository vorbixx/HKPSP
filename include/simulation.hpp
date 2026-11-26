#pragma once
#include "animation.hpp"
#include "combat.hpp"
#include "enemy.hpp"
#include "hero_tuning.hpp"
#include "game_clock.hpp"
#include "room_entities.hpp"
#include "geo.hpp"
#include "presentation.hpp"
#include "shade.hpp"
#include "spells.hpp"
#include "performance_sections.hpp"
#include <algorithm>

namespace hk3ds
{
struct RoomActors
{
    Buzzer* buzzers=nullptr;
    std::size_t buzzerCount=0;
    Stalactite* stalactites=nullptr;
    std::size_t stalactiteCount=0;
    GeoBank* geo=nullptr;
    Rect heroBox=bodyBounds({});
    void* contactContext=nullptr;
    void (*afterPhysics)(void*)=nullptr;
    HollowShade* shade=nullptr;
    // Original frame fixtures disable HeroController and Rigidbody2D together.
    bool heroSimulated=true;
    CombatScene* encounterCombat=nullptr;
    // Room-owned actors may schedule nail combat themselves.
    CombatScene* spellTargets=nullptr;
    void (*beforePhysics)(void*)=nullptr;
    void (*spellGeometry)(void*,Rect)=nullptr;
};
class Simulation
{
    friend struct SimulationStateHash;
public:
    Hero hero;
    SpiritProjectiles spirits;
    // Observation only: 1.. crawlers, 1001.. buzzers, 2001.. stalactites.
    unsigned lastDamageSource=0;
    HeroAnimator animator;
    GameClock clock;
    FrameClock frameTime;
    float scaledSeconds=0;
    std::uint64_t controlTicks = 0, physicsTicks = 0, overloads = 0;
    // Scene actors have a shorter lifetime than the engine's fixed schedule
    // and GameManager freeze coroutines. Every reload uses this boundary.
    void resetScene()
    {
        const auto savedClock=clock;
        const auto savedFrameTime=frameTime;
        const auto savedControlTicks=controlTicks,savedPhysicsTicks=physicsTicks,savedOverloads=overloads;
        const auto savedAccumulator=controlAccumulator_;
        *this={};
        clock=savedClock;frameTime=savedFrameTime;
        controlTicks=savedControlTicks;physicsTicks=savedPhysicsTicks;overloads=savedOverloads;
        controlAccumulator_=savedAccumulator;
        hero.scale=clock.scale;
    }
    PresentationTime presentationTime(double pendingSeconds=-1) const
    {
        const double pending=pendingSeconds<0?controlAccumulator_:pendingSeconds;
        return {static_cast<float>((frameTime.remainder+
            pending*hero.scale-tuning::controlStep)/tuning::physicsStep)};
    }
    Vec2 heroRenderPosition(double pendingSeconds=-1) const
    { return hero.renderPosition(presentationTime(pendingSeconds)); }
    // Fixed control schedule is intentional for repeatable 3DS behavior.
    // Update and physics remain independently callable for reference traces.
    void advance(double seconds, InputLatch& input, const World& world, const AnimationBank& bank, CombatScene* combat = nullptr, Crawler* crawler = nullptr, std::size_t crawlerCount = 1, RoomActors roomActors = {})
    {
        if(!std::isfinite(seconds)){++overloads;return;}
        if (seconds < 0 || seconds > tuning::controlStep * 5) ++overloads;
        controlAccumulator_ += std::clamp(seconds, 0.0, tuning::controlStep * 5);
        for (int i = 0; i < 5 && controlAccumulator_ + 1e-12 >= tuning::controlStep; ++i)
        {
            updateFrame(tuning::controlStep,input,world,bank,combat,crawler,crawlerCount,roomActors);
            controlAccumulator_ -= tuning::controlStep;
        }
    }
    // One Update/continuation cycle; the adapter above supplies bounded 60 Hz
    // frames. Reference controls can supply measured Update intervals directly.
    void updateFrame(double seconds, InputLatch& input, const World& world, const AnimationBank& bank, CombatScene* combat = nullptr, Crawler* crawler = nullptr, std::size_t crawlerCount = 1, RoomActors roomActors = {})
    {
            SimulationScope scheduleTiming(PerformanceSection::SimSchedule);
            if(!std::isfinite(seconds)||seconds<0||seconds>tuning::controlStep*5){++overloads;return;}
            if (!crawler) crawlerCount=0;
            if (crawlerCount>CombatScene::capacity) { ++overloads; return; }
            if((roomActors.buzzerCount&&!roomActors.buzzers)||(roomActors.stalactiteCount&&!roomActors.stalactites)||
               roomActors.buzzerCount>RoomEntityBank::maxBuzzers||roomActors.stalactiteCount>RoomEntityBank::maxStalactites)
            {++overloads;return;}
            clock.beginFrame();
            syncDamageFreeze();
            const float scaled = frameTime.advance(static_cast<float>(seconds),clock.scale);
            scaledSeconds=scaled;
            while (frameTime.nextFixed(tuning::physicsStep))
            {
                if(roomActors.beforePhysics){SimulationScope timing(PerformanceSection::SimPhysicsCallbacks);roomActors.beforePhysics(roomActors.contactContext);}
                if(roomActors.heroSimulated){PerformanceScope timing(PerformanceSection::SimHero);hero.fixedUpdate(static_cast<float>(tuning::physicsStep), world);}
                if(roomActors.geo){PerformanceScope timing(PerformanceSection::SimParticles);roomActors.geo->fixedUpdate(static_cast<float>(tuning::physicsStep),world);}
                {PerformanceScope timing(PerformanceSection::SimEnemies);
                for (std::size_t i=0;i<crawlerCount;++i) crawler[i].fixedUpdate(static_cast<float>(tuning::physicsStep),world,false);
                for(std::size_t i=0;i<roomActors.buzzerCount;++i)roomActors.buzzers[i].fixedUpdate(static_cast<float>(tuning::physicsStep),hero,world);
                for(std::size_t i=0;i<roomActors.stalactiteCount;++i)roomActors.stalactites[i].fixedUpdate(static_cast<float>(tuning::physicsStep),world);
                }
                if(roomActors.afterPhysics){SimulationScope timing(PerformanceSection::SimPhysicsCallbacks);roomActors.afterPhysics(roomActors.contactContext);}
                {SimulationScope timing(PerformanceSection::SimContacts);
                if(roomActors.shade&&roomActors.heroSimulated){const int health=hero.vitals.health;
                    roomActors.shade->fixedUpdate(static_cast<float>(tuning::physicsStep),hero,world);
                    if(hero.vitals.health<health)lastDamageSource=3001;
                    syncDamageFreeze();}
                if (combat&&roomActors.heroSimulated) combat->fixedUpdate(hero);
                if(roomActors.heroSimulated){
                    if(combat)combat->superDashHit(hero);
                    if(roomActors.encounterCombat&&roomActors.encounterCombat!=combat)roomActors.encounterCombat->superDashHit(hero);
                    if(roomActors.spellTargets&&roomActors.spellTargets!=combat&&roomActors.spellTargets!=roomActors.encounterCombat)
                        roomActors.spellTargets->superDashHit(hero);
                }
                if(roomActors.heroSimulated)spirits.fixedUpdate(static_cast<float>(tuning::physicsStep),hero,combat,roomActors.encounterCombat,roomActors.spellTargets,roomActors.spellGeometry,roomActors.contactContext);
                if(roomActors.geo&&roomActors.heroSimulated){
                    if(hero.charms.gatheringSwarm)roomActors.geo->gather(hero.position,float(tuning::physicsStep));
                    roomActors.geo->contactHero(boxBounds(hero.position,roomActors.heroBox,hero.facingRight?-1.0f:1.0f));
                }
                for (std::size_t i=0;i<crawlerCount;++i)
                {
                    auto& enemy=crawler[i];
                    if(enemy.killFreezeRequested)
                    {clock.enemyKill();enemy.killFreezeRequested=false;}
                    const int health=hero.vitals.health;
                    if(roomActors.heroSimulated)enemy.contactHero(hero);
                    if(hero.vitals.health<health)lastDamageSource=1+static_cast<unsigned>(i);
                    syncDamageFreeze();
                    enemy.afterPhysics(world);
                }
                for(std::size_t i=0;i<roomActors.buzzerCount;++i)
                {
                    auto& enemy=roomActors.buzzers[i];
                    if(enemy.killFreezeRequested){clock.enemyKill();enemy.killFreezeRequested=false;}
                    const int health=hero.vitals.health;
                    if(roomActors.heroSimulated)enemy.contactHero(hero);
                    syncDamageFreeze();
                    if(hero.vitals.health<health)lastDamageSource=1001+static_cast<unsigned>(i);
                }
                for(std::size_t i=0;i<roomActors.stalactiteCount;++i)
                {const int health=hero.vitals.health;if(roomActors.heroSimulated)roomActors.stalactites[i].contactHero(hero);syncDamageFreeze();
                 if(hero.vitals.health<health)lastDamageSource=2001+static_cast<unsigned>(i);}
                ++physicsTicks;
                }
            }
            for (std::size_t i=0;i<crawlerCount;++i) crawler[i].interpolate(static_cast<float>(frameTime.remainder/tuning::physicsStep));
            {SimulationScope timing(PerformanceSection::SimCombat);
            if (combat&&roomActors.heroSimulated) combat->update(hero, static_cast<float>(scaled));
            if(combat&&roomActors.heroSimulated)combat->charmEffects(hero,static_cast<float>(scaled));
            }
            if(roomActors.geo){PerformanceScope timing(PerformanceSection::SimParticles);roomActors.geo->update(scaled);}
            const auto frameInput=input.consume();
            if(roomActors.heroSimulated){
                PerformanceScope timing(PerformanceSection::SimHero);
                hero.update(frameInput, static_cast<float>(scaled),world);
                spirits.observe(hero);
                hero.checkTerrainThunk(static_cast<float>(scaled),world);
            }
            {PerformanceScope timing(PerformanceSection::SimEnemies);
            for (std::size_t i=0;i<crawlerCount;++i) crawler[i].update(static_cast<float>(scaled),static_cast<float>(seconds),world);
            for(std::size_t i=0;i<roomActors.buzzerCount;++i)roomActors.buzzers[i].update(scaled,hero,world);
            for(std::size_t i=0;i<roomActors.stalactiteCount;++i)roomActors.stalactites[i].update(scaled,hero,world);
            }
            {PerformanceScope timing(PerformanceSection::SimHero);animator.update(hero, bank, static_cast<float>(scaled), combat ? combat->bank() : nullptr);}
            clock.afterUpdate(static_cast<float>(seconds));
            if(roomActors.heroSimulated){PerformanceScope timing(PerformanceSection::SimHero);hero.resumeCoroutines(static_cast<float>(scaled));}
            if(damageFreeze_&&!clock.active(damageFreeze_)){hero.freezePhase=0;damageFreeze_=0;}
            hero.scale=clock.scale;
            ++controlTicks;
    }
// Room hazards can kill after the simulation Update has completed.
    // Enroll the same one-shot owner in that frame for every lethal source.
    void syncDeathFreeze()
    {
        if(hero.actor!=Actor::Dead){deathFreezeStarted_=false;return;}
        if(!deathFreezeStarted_){clock.heroDeath();deathFreezeStarted_=true;}
    }
private:
    double controlAccumulator_ = 0;
    std::uint64_t damageFreeze_=0;
    bool deathFreezeStarted_=false;
    void syncDamageFreeze()
    {
        // All lethal sources (enemy, combat receiver, hazard) share this edge.
        syncDeathFreeze();
        if(hero.freezePhase&&!damageFreeze_)damageFreeze_=clock.heroDamage();
    }
};
}
