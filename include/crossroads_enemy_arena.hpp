#pragma once
#include "crossroads_enemy.hpp"
#include "renderer.hpp"
#include "tutorial.hpp"
#include <algorithm>
#include <cstdio>

namespace hk3ds {
// Explicit test adapter only. Production scenes own actors/banks themselves.
class CrossroadsEnemyArena {
public:
    void select(unsigned serial,int kind,TutorialSession& game,const NailBank& nails,const World& world)
    {
        if(serial==serial_)return;
        serial_=serial;renderer_.release();actor_=CrossroadsEnemy{};active_=false;released_=0;elapsed_=0;
        if(kind<0||kind>=4)return;
        const auto& entry=crossroadsEnemyRegistry[kind];char path[128],prefix[64];
        std::snprintf(path,sizeof(path),"romfs:/%s.bin",entry.assetStem);CrossroadsEnemyConfig config;
        if(!config.load(path))return;
        std::snprintf(path,sizeof(path),"romfs:/%s-anim.bin",entry.assetStem);
        std::snprintf(prefix,sizeof(prefix),"%s-page-",entry.assetStem);
        if(!bank_.load(path)||!renderer_.load("romfs:",bank_,prefix))return;
        auto spawn=game.simulation.hero.position;spawn.x+=2;spawn.y+=.2f;
        if(config.kind==CrossroadsEnemyKind::Tiktik){float distance=0;
            if(rayTerrain(spawn,{0,-1},8,world,distance))spawn.y-=distance+config.flight.body.bottom;
        }
        active_=actor_.bind(config,bank_,object_,spawn,world,{&seed_,randomUnit},config.kind==CrossroadsEnemyKind::Tiktik?-1.0f:1.0f);
        if(active_)active_=combat_.bind(&object_,1,&nails);
        std::printf("Enemy A arena: %s %s\n",entry.prefab,active_?"ready":"failed");
    }
    void advance(double seconds,TutorialSession& game,const World& world)
    {
        if(!active_)return;
        elapsed_+=std::clamp(seconds,0.0,.1)*game.simulation.clock.scale;
        while(elapsed_>=.02){elapsed_-=.02;auto& hero=game.simulation.hero;
            actor_.update(.02f,hero,world);combat_.update(hero,.02f);actor_.fixedUpdate(.02f,hero,world);
            combat_.fixedUpdate(hero);actor_.contactHero(hero);
            if(actor_.geoReleased>released_){game.coins.spawn(actor_.geoReleased-released_,JournalEnemy::Buzzer,actor_.position);released_=actor_.geoReleased;}
        }
    }
    void draw(float cameraX,float cameraY,float ppu)const
    {
        if(!active_||!actor_.visible())return;
        const auto p=actor_.renderPosition({static_cast<float>(elapsed_/.02)});
        renderer_.drawAnimation(actor_.animation(),bank_,200+(p.x-cameraX)*ppu,120-(p.y-cameraY)*ppu,
            ppu,actor_.artScale(),1,1,actor_.rotation);
    }
    ActorAudioState audio(Vec2 listener)const
    {auto state=actor_.audioState(EnemyAudioEvents::ArenaOwner,listener);state.active=active_;return state;}
    bool active()const{return active_;}
    void release(){renderer_.release();active_=false;}
private:
    static float randomUnit(void* context){auto& seed=*static_cast<unsigned*>(context);seed=seed*1664525u+1013904223u;return static_cast<float>(seed>>8)/16777216.0f;}
    CrossroadsEnemy actor_;CombatObject object_;CombatScene combat_;
    AnimationBank bank_;HeroRenderer renderer_;
    unsigned serial_=0,seed_=0x3d501u,released_=0;
    double elapsed_=0;bool active_=false;
};
}
