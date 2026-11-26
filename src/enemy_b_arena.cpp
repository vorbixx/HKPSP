#ifdef __3DS__
#include "enemy_b_arena.hpp"
#include "enemy_b.hpp"
#include "renderer.hpp"
#include <cstdio>

namespace hk3ds
{
namespace
{
AnimationBank arenaBank;
HeroRenderer arenaRenderer;
EnemyBConfigs arenaConfigs;
std::array<EnemyB,4> arenaActors;
std::array<CombatObject,4> arenaObjects;
bool arenaReady=false;
}
void loadEnemyBArena()
{
    auto* marker=std::fopen("romfs:/enemy-b-arena.txt","rb");if(!marker)return;
    std::fclose(marker);
    if(!loadEnemyBConfigs("romfs:/enemy-b.bin",arenaConfigs)||!arenaBank.load("romfs:/enemy-b-anim.bin")||
       !arenaRenderer.load("romfs:",arenaBank,"enemy-b-page-"))return;
    const Solid floor[]={{{-100,-2,100,0}}};const World world{floor,1};
    for(unsigned i=0;i<4;++i)
    {
        const Vec2 spawn{static_cast<float>(i)*5,-arenaConfigs[i].body.bottom};
        if(!arenaActors[i].bind(static_cast<EnemyBKind>(i),arenaConfigs[i],arenaBank,arenaObjects[i],spawn))return;
        const unsigned ticks=i==3?40:30;
        for(unsigned tick=0;tick<ticks;++tick)arenaActors[i].fixedUpdate(.02f,{spawn.x-3,spawn.y},world);
    }
    arenaReady=true;
}
bool drawEnemyBArena()
{
    if(!arenaReady)return false;
    C2D_DrawRectSolid(0,0,0,400,240,C2D_Color32(24,28,40,255));
    for(unsigned i=0;i<4;++i)
    {
        C2D_DrawRectSolid(i*100,210,0,99,2,C2D_Color32(100,110,125,255));
        const auto& actor=arenaActors[i];
        if(actor.visible())arenaRenderer.drawAnimation(actor.animation,arenaBank,50+i*100,
            210-actor.position.y*24,24,actor.artScale());
    }
    return true;
}
}
#endif
