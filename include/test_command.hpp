#pragma once
#include "tutorial.hpp"
#include <cmath>
#include <cstdint>

namespace hk3ds
{
inline constexpr std::uint32_t mantisSpellCommand = 24;
// RPC ABI: publish payload first, sequence last; ack is written after execution.
struct TestCommand
{
    std::uint32_t magic=0,sequence=0,op=0;
    float args[4]{};
    std::uint32_t ack=0,result=0,decorBreaks=0;
};
static_assert(sizeof(TestCommand)==40,"test command RPC ABI");
struct TestCommandInput
{
    bool active=false;
    float x=0,y=0;
    std::uint16_t held=0;
    int enemyArena=-1;
    unsigned enemyArenaSerial=0;
    void sample(InputLatch& input) const { input.sample(x,y,held); }
};
inline void pollTestCommand(volatile TestCommand& box,TutorialSession& game,bool enabled,TestCommandInput* input=nullptr)
{
    if(!enabled||box.magic!=0x484b5443u)return;
    box.decorBreaks=game.testDecorBreaks();
    const auto sequence=box.sequence;
    if(!sequence||sequence==box.ack)return;
    const auto op=box.op;const float x=box.args[0],y=box.args[1];
    std::uint32_t result=0;
    if(op==1&&std::isfinite(x)&&std::isfinite(y))
    {
        auto& h=game.simulation.hero;h.position={x,y};h.velocity={};h.damageVelocity={};
        h.presentation.reset(h.position);h.contacts={};
    }
    else if(op==21&&x==1&&game.uumuu.active()&&
            (y==0||y==5||y==9||y==13||y==17||y==21)){
        // Explicit capture state: position and optional source nail upgrade.
        // The replay still swings the real nail through combat/health guards.
        auto& h=game.simulation.hero;const auto p=game.uumuu.position();
        if(y!=0)h.nailDamage=static_cast<int>(y);
        h.position={p.x-1.5f,p.y};h.velocity={};h.damageVelocity={};h.contacts={};
        h.presentation.reset(h.position);h.facingRight=true;
        box.args[2]=static_cast<float>(game.uumuu.hp()); // Capture feedback after positioning.
        box.args[3]=14766; // Feedback ABI tag; reject stale diagnostic builds.
    }
    else if(op==2&&(x==0||x==1))game.testFreezeEnemies=x!=0;
    else if(op==3&&(x==0||x==1))game.testInvulnerable=x!=0;
    else if(op==4&&std::isfinite(x)&&x>=0&&x<16777216&&std::floor(x)==x&&
            std::isfinite(y)&&y>=0&&y<16777216&&std::floor(y)==y&&(box.args[2]==0||box.args[2]==1))
    {
        // Optional cosmetic seed for repeatable effect captures; zero preserves play RNG.
        if(y>0)game.decorEffects.reset(static_cast<unsigned>(y));
        if(!game.testBreakDecor(static_cast<unsigned>(x),box.args[2]==1?AttackType::Spell:AttackType::Nail))result=2;
    }
    else if(op==5&&std::isfinite(x)&&x>=0&&x<=7&&std::floor(x)==x&&(y==0||y==1))
    {game.testDecorLayers=static_cast<unsigned>(x);game.testIsolateDecor=y!=0;}
    else if(op==6&&input&&std::isfinite(x)&&std::isfinite(y)&&std::abs(x)<=1&&std::abs(y)<=1&&
            std::isfinite(box.args[2])&&box.args[2]>=0&&box.args[2]<=255&&std::floor(box.args[2])==box.args[2])
    {input->active=true;input->x=x;input->y=y;input->held=static_cast<std::uint16_t>(box.args[2]);}
    else if(op==7&&(x==1||x==2)){
        // Deliver the tablet FSM's presentation events; never override alpha.
        if(x==1)game.prompts.beginTabletFocus();else game.prompts.endTabletFocus();
    }
    else if(op==8&&std::isfinite(x)&&x>=1&&x<=99&&std::floor(x)==x)
    {if(!game.simulation.hero.takeDamage(static_cast<int>(x),1))result=2;}
    else if(op==9&&x==4){
        // Rescued-Sly visual fixture, reachable only with the explicit test adapter.
        if(!game.setPlayerData("slyRescued",1))result=2;
    }
    else if(op==9&&(x==0||x==1||x==2||x==3)){
        // Prepared late-game save, reachable only with the explicit test adapter.
        auto save=game.saved();save.conditional={};
        if(x==1||x==3){save.conditional.mrMushroomState=7;save.conditional.flags|=ConditionalSave::SporeShroom|ConditionalSave::DreamNail;}
        if(x==2||x==3){save.conditional.flags|=ConditionalSave::Grimmchild;save.conditional.grimmChildLevel=2;save.conditional.flamesRequired=3;}
        if(!game.restore(save))result=2;
        else {game.simulation.hero.controlEnabled=true;game.simulation.hero.gravityEnabled=true;game.simulation.animator.overrideClip=nullptr;}
    }
    else if(op==10&&(x==1||x==2)){
        // Move the test hero into nail range; damage still uses the live nail.
        auto& h=game.simulation.hero;const auto& e=game.conditionalEvents;
        const auto p=x==1?e.mushroomPosition:e.grimmkinPosition;
        h.position={p.x-1.2f,p.y};h.velocity={};h.damageVelocity={};h.contacts={};h.facingRight=true;
        h.presentation.reset(h.position);
    }
    else if(op==11&&(x==0||x==1)){
        // Prepared source visitedCrossroads save; restore runs the real entry clock.
        auto save=game.saved();
        save.flags=(save.flags&~AreaTitle::visitedCrossroadsFlag)|(x!=0?AreaTitle::visitedCrossroadsFlag:0u);
        if(!game.restore(save))result=2;
    }
    else if(op==12&&std::isfinite(x)){
        // Aim a capture at the live randomly flung shiny, without changing it.
        auto& h=game.simulation.hero;h.position={game.furyChest.shinyPosition().x,x};
        h.velocity={};h.damageVelocity={};h.contacts={};h.presentation.reset(h.position);
    }
    else if(op==14&&(x==0||x==1)){
        // Presentation fixture only: ordinary dash ability, with gameplay acceptance intact.
        game.simulation.hero.abilities.dash=x!=0;
    }
    else if(op==13&&std::isfinite(x)&&std::isfinite(y)&&std::isfinite(box.args[2])){
        const auto& c=game.furyChest;const float flags=box.args[2];
        if((x>=0&&x!=int(c.phase()))||(y>=0&&y!=int(c.pickup()))||(flags>=0&&flags!=c.flags()))result=3;
    }
    else if(op==23&&(x==0||x==1)&&(y==0||y==1)){
        // Observe the live lift; this must never activate or repair a fixture.
        if(!game.townLift.bound()||game.townLift.closedCageHidden()!=(x!=0)||
           game.townLift.openCageVisible()!=(y!=0))result=3;
    }
    else if(op==15&&(x==0||x==1))game.testPinnedHeroTriggers=x!=0;
    else if(op==16){if(!game.restore(game.saved()))result=2;}
    else if(op==19&&std::isfinite(x)&&x>=0&&x<5&&x!=3&&std::floor(x)==x){
        game.enemyC.clear();
        const auto kind=static_cast<EnemyCKind>(unsigned(x));
        const auto h=game.simulation.hero.position;
        const bool flying=kind==EnemyCKind::AspidMother||kind==EnemyCKind::GlowingWomb;
        const float y=flying?h.y+2:bodyBounds(h).bottom-enemyCProfile(kind).body.bottom;
        if(!game.enemyC.bank()||!game.enemyC.spawn(kind,{h.x+4,y}))result=2;
    }
    else if(op==18&&input&&std::isfinite(x)&&x>=0&&x<=4&&std::floor(x)==x)
    {input->enemyArena=x==4?-1:static_cast<int>(x);++input->enemyArenaSerial;}
    else if(op==20&&std::isfinite(x)&&x>=0&&x<=9&&std::floor(x)==x){if(!game.startGreenpathArena(unsigned(x)))result=2;}
    else if(op==21&&x==20){if(!game.testWorldQuestRoom())result=2;}
    else if(op==17&&(x==1||x==2)){if(!game.startBossArena(static_cast<unsigned>(x)))result=2;}
    else if(op==mantisSpellCommand&&std::isfinite(x)&&x>=1&&x<=1000&&std::floor(x)==x){
        // Explicit visual fixture: progress the real encounter through spell hits.
        // Hidden/invincible Lords still reject damage through the combat system.
        auto& encounter=game.sceneEnemies.mantis;
        if(!encounter.active()||!encounter.locked())result=2;
        else {
            std::array<bool,CombatScene::capacity> hit{};
            game.sceneEnemies.combat().spellHit(MantisLords::arena(),Strike::Right,hit,
                                                game.simulation.hero,static_cast<int>(x));
        }
    }
    // 22 is reserved by the ARM adapter for capture-only scene travel.
    else if(op==23&&std::isfinite(x)&&x>=1&&x<=99&&std::floor(x)==x){
        // Replay fixture: damage, recoil and ordinary attacks remain enabled.
        auto& v=game.simulation.hero.vitals;v.health=static_cast<int>(x);
        v.maxHealth=std::max(v.maxHealth,v.health);
    }
    else if(op==128&&x==1){if(!game.restart(true))result=2;}
    else if((op==129||op==130)&&std::isfinite(x)&&std::isfinite(y)&&
            std::isfinite(box.args[2])&&x>=1&&x<=99&&y>=0&&y<=99&&
            box.args[2]>=0&&box.args[2]<=99&&std::floor(x)==x&&std::floor(y)==y&&
            std::floor(box.args[2])==box.args[2]){
        auto& v=game.simulation.hero.vitals;
        if(op==129){
            // One-time fixture input; subsequent scene/retry outcomes stay live.
            if(x>v.maxHealth||y>v.maxSoul||box.args[2]>v.reserveMax)result=2;
            else {v.health=int(x);v.soul=int(y);v.reserve=int(box.args[2]);}
        }else if(v.health!=int(x)||v.soul!=int(y)||v.reserve!=int(box.args[2])||
                 v.blueHealth!=box.args[3])result=3;
    }
    else if(op==131&&x>=1&&x<=40&&std::floor(x)==x&&(y==0||y==1)&&
            (box.args[2]==0||box.args[2]==1)){
        if(game.charmInventory.wears(unsigned(x))!=(y!=0)||
           game.simulation.hero.charms.sprint!=(box.args[2]!=0))result=3;
    }
    else if(op==132&&(x==0||x==1)&&(y==0||y==1)&&
            (box.args[2]==0||box.args[2]==1)&&std::isfinite(box.args[3])&&
            box.args[3]>=0&&box.args[3]<=9&&std::floor(box.args[3])==box.args[3]){
        // Observe real interaction outcomes; never grant a key or open the gate.
        const auto& data=game.playerData();
        if(!game.cityCrest.bound()||(data.get("openedCityGate")!=0)!=(x!=0)||
           (data.get("cityGateClosed")!=0)!=(y!=0)||(data.get("hasCityKey")!=0)!=(box.args[2]!=0)||
           int(game.cityCrest.phase())!=int(box.args[3]))result=3;
        if(y!=0&&(data.get("mapZone")!=int(game.cityCrest.data().mapZone)||
           std::strcmp(game.resting.saved.benchScene,"Fungus2_21")||
           game.resting.saved.benchX!=game.cityCrest.data().respawn.x||
           game.resting.saved.benchY!=game.cityCrest.data().respawn.y))result=3;
    }
    else if(op==133&&std::isfinite(x)&&x>=1&&x<=1000000&&std::floor(x)==x&&
            (y==0||y==1)&&(box.args[2]==0||box.args[2]==1)&&(box.args[3]==0||box.args[3]==1)){
        // Read the actual hit/save/collision state; this command changes nothing.
        if(!game.testSceneLeverState(static_cast<unsigned>(x),y!=0,box.args[2]!=0,box.args[3]!=0))result=3;
    }
    else result=1;
    box.result=result;box.ack=sequence;
}
}
