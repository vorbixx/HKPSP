#pragma once
#include "hero.hpp"
#include "hero_tuning.hpp"
#include <cstdint>
#include <cstring>
#include <limits>

namespace hk3ds {
struct TraversalCheckpoint { Vec2 position; bool facingRight; };
struct TraversalReward {
    AbilityReward reward;
    const char* scene;
    std::uint32_t sourceId;
    Vec2 position;
    float touchRadius;
    const unsigned* visualOwners;
    unsigned visualOwnerCount;
    Rect inspectBounds{};
    Rect collisionBounds{};
    const TraversalCheckpoint* checkpoint=nullptr;
};
// Steam build 22529139: source level number * 1000000 + GameObject path ID,
// matching the existing collectible ledger. World transforms retain source scale.
inline constexpr unsigned clawPickupOwners[]={796,791,793,792,795};
inline constexpr unsigned wingsPickupOwners[]={540,538,539,533,537,534,532,541,536,535};
inline constexpr unsigned heartPickupOwners[]={1355,1356,1357,1358,1354,1364,1363,1362,1360,1353,1359,1361,1934};
inline constexpr unsigned ismaPickupOwners[]={199,200,197,198,201,202,196,1065,1966};
// Death Respawn Marker743 / Transform2053 / RespawnMarker4168, spawnType0.
// This is the downward ray origin, rather than the final hero position.
inline constexpr TraversalCheckpoint shadeCloakCheckpoint{{99.35899353f,12.32896519f},false};
inline constexpr TraversalReward traversalRewards[]={
    // Inspect Region791 / trigger5116, including Shiny Item Stand796 scale1.5.
    {AbilityReward::MantisClaw,"Fungus2_14",178000796,{18.39629555f,20.29000092f},0,clawPickupOwners,5,
     {17.14006910f,20.11722315f,19.65252200f,20.49762030f}},
    // CircleCollider2D 2143 radius .2, root scale 1.5; DJ Control 3522 ENTER.
    {AbilityReward::MonarchWings,"Abyss_21",345000540,{66.30999756f,232.80000305f},.3f,wingsPickupOwners,10},
    // Inspect Region 1356 / collider 4308, including the authored parent scale.
    // Root 1355 FSM destroys its whole hierarchy after hasSuperDash is saved.
    {AbilityReward::CrystalHeart,"Mines_31",270001355,{243.68000488f,8.24000001f},0,heartPickupOwners,13,
     {242.42377843f,8.06722316f,244.93623133f,8.44762031f}},
    // Waterways_13 Shiny Item Acid 199 / trigger 4530 / Control FSM 6585.
    {AbilityReward::IsmasTear,"Waterways_13",362000199,{94.01f,17.88f},0,ismaPickupOwners,9,
     {92.855f,16.81841675f,95.165f,17.26507706f}},
    // Dish Plat455 / solid collider2706 / Get Shadow Dash4149, level337.
    // BuildSettings.m_Scenes[337] is Abyss_10; Got leaves the dish intact.
    {AbilityReward::ShadeCloak,"Abyss_10",337000455,{102.13f,17.07f},0,nullptr,0,{},
     {100.57587441f,16.85500748f,103.87078541f,17.11292488f},&shadeCloakCheckpoint},
    // Ruins Shaman989 / inspect trigger3762 / FSM5987; Get Fireball5826 sets level2.
    // The source keeps the figure, changing its sprite after the acquisition.
    {AbilityReward::ShadeSoul,"Ruins1_31b",109000989,{34.75475693f,57.22618484f},0,nullptr,0,
     {32.94862700f,55.03868385f,37.04148530f,55.31611343f}},
};
inline const TraversalReward* traversalReward(AbilityReward reward) {
    for(const auto& source:traversalRewards)if(source.reward==reward)return &source;
    return nullptr;
}
inline const TraversalReward* traversalReward(const char* scene) {
    if(scene)for(const auto& source:traversalRewards)if(!std::strcmp(source.scene,scene))return &source;
    return nullptr;
}
inline void placeTraversalCheckpoint(Hero& hero,const World& world,const TraversalCheckpoint& checkpoint) {
    hero.position=checkpoint.position;
    float distance=0;
    // HeroController.FindGroundPoint: layer8 ray, then collider half height,
    // minus offset, plus .01. A finite maximum avoids 0*infinity in geometry.
    if(rayTerrain(checkpoint.position,{0,-1},std::numeric_limits<float>::max(),world,distance,TerrainShape::terrainQueryExcluded))
        hero.position.y+=-distance+tuning::bodyHeight/2-tuning::bodyOffsetY+.01f;
    hero.facingRight=checkpoint.facingRight;
    hero.velocity={};hero.contacts={};hero.contacts.ground=true;hero.actor=Actor::Idle;
}
}
