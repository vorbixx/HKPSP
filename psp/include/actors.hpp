#pragma once
#include "draw_list.hpp"
#include "tutorial.hpp"
#include "scene_enemies.hpp"
#include <memory>
#include <cstdio>

namespace hkpsp {
class Platform;
using ActorAtlas=AnimationAtlas;
struct ActorArt {
    ActorAtlas crawler,buzzer,shade,geo,door,fury,decor,events;
    void load(Platform&,std::vector<Texture>&,AnimationPages&);
    void bindTerrain(Platform&,hk3ds::TutorialSession&,hk3ds::TerrainBank&);
    void bindEffects(Platform&,hk3ds::TutorialSession&,const hk3ds::AnimationBank&,const hk3ds::GrassBank&);
};
void appendDoor(DrawList&,const ActorArt&,const hk3ds::TutorialSession&,hk3ds::Vec2);
void appendGeo(DrawList&,const ActorArt&,const hk3ds::TutorialSession&,hk3ds::Vec2);
void appendFury(DrawList&,const ActorArt&,const hk3ds::TutorialSession&,hk3ds::Vec2,bool front);
void appendConditional(DrawList&,const ActorArt&,const hk3ds::TutorialSession&,hk3ds::Vec2);
void appendCrawlers(DrawList&,const ActorArt&,const hk3ds::TutorialSession&,hk3ds::Vec2,std::FILE* survey=nullptr);
// Scene enemies (Tiktik, Gruzzer, Vengefly, Aspid, Enemy B, Enemy C, Elder Baldur) use PSA1 render banks loaded per scene.
// Simulation bank index: 1 Tiktik, 2 Gruzzer, 3 Vengefly, 4 Aspid, 5 Enemy B, 6 Enemy C, 7 encounter art, 8 Elder Baldur (SceneEnemies::bankCount).
struct SceneEnemyArt {
    static constexpr unsigned slots=hk3ds::SceneEnemies::bankCount;
    std::array<ActorAtlas,slots> banks{};
    std::array<bool,slots> ready{};
    std::unique_ptr<hk3ds::SceneEnemyRenderState> state; // heap: 128 actor records stay off the main stack
    void clear();
    // Loads only the banks the scene's simulation uses.
    void load(Platform&,std::vector<Texture>&,AnimationPages&,const hk3ds::SceneEnemies&);
};
void appendSceneEnemies(DrawList&,SceneEnemyArt&,const ActorAtlas& crawler,const hk3ds::TutorialSession&,hk3ds::Vec2 camera,std::FILE* survey=nullptr);
struct GreenpathArt {
    ActorAtlas atlas;
    void clear(hk3ds::GreenpathEncounter&);
    void load(Platform&,std::vector<Texture>&,AnimationPages&,hk3ds::GreenpathEncounter&,
              const hk3ds::NailBank&,const std::string& metadata,std::uint32_t flags);
};
void appendGreenpath(DrawList&,const GreenpathArt&,const hk3ds::TutorialSession&,hk3ds::Vec2 camera,std::FILE* survey=nullptr);
void appendShade(DrawList&,const ActorArt&,const hk3ds::TutorialSession&,hk3ds::Vec2);
void appendBuzzers(DrawList&,const ActorArt&,const hk3ds::TutorialSession&,hk3ds::Vec2,std::FILE* survey=nullptr);
void appendStagStation(DrawList&,const hk3ds::AnimationBank&,const hk3ds::TutorialSession&,hk3ds::PresentationTime,hk3ds::Vec2 camera);
void appendSlash(DrawList&,const hk3ds::AnimationBank&,unsigned,const hk3ds::TutorialSession&,const hk3ds::NailBank&,hk3ds::Vec2,hk3ds::Vec2);
void appendFocus(DrawList&,const hk3ds::AnimationBank&,unsigned,const hk3ds::TutorialSession&,hk3ds::Vec2,hk3ds::FocusDrawPass);
void appendHeroEffects(DrawList&,const hk3ds::AnimationBank&,unsigned,const hk3ds::TutorialSession&,hk3ds::Vec2);
// Cast Vengeful Spirit projectiles with the original Ball clips (src/main.cpp:873-885).
void appendSpiritProjectile(DrawList&,const hk3ds::AnimationBank&,unsigned,const hk3ds::SpiritProjectile&,
                            hk3ds::PresentationTime,hk3ds::Vec2);
void appendSpirits(DrawList&,const hk3ds::AnimationBank&,unsigned,const hk3ds::TutorialSession&,hk3ds::Vec2);
void appendDecor(hk3ds::TutorialSession&);
}
