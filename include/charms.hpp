#pragma once
#include "hero.hpp"
#include <algorithm>
#include <array>
#include <cstring>

namespace hk3ds {
struct CharmDefinition { unsigned id,cost,price; const char* name; const char* description; };
// PlayerData charm IDs. This slice includes the early shops and room rewards.
inline constexpr std::array<CharmDefinition,15> charmDefinitions{{
    {1,1,300,"Gathering Swarm","@UI/CHARM_DESC_1"},
    {2,1,220,"Wayward Compass","Shows your position on the map."},
    {3,1,0,"Grubsong","@UI/CHARM_DESC_3"},
    {4,2,200,"Stalwart Shell","Longer recovery and invulnerability."},
    {5,2,0,"Baldur Shell","Protects you while focusing."},
    {6,2,0,"Fury of the Fallen","Stronger nail at one mask."},
    {7,3,800,"Quick Focus","Focus Soul more quickly."},
    {8,2,250,"Lifeblood Heart","Two blue masks when equipped."},
    {12,1,0,"Thorns of Agony","Strike nearby foes when hurt."},
    {14,1,120,"Steady Body","No recoil from nail strikes."},
    {18,2,300,"Longnail","Increases nail reach."},
    {19,3,220,"Shaman Stone","Increases spell power."},
    {20,2,0,"Soul Catcher","More Soul from nail strikes."},
    {22,2,0,"Glowing Womb","Soul feeds attacking hatchlings."},
    {10,1,0,"Defender's Crest","Emits a heroic odour."},
}};
constexpr std::uint32_t charmBit(unsigned id) { return id&&id<=32?1u<<(id-1):0; }
struct CharmInventory {
    std::uint32_t owned=0,equipped=0,notches=3;
    static constexpr std::uint32_t mask=charmBit(1)|charmBit(2)|charmBit(3)|charmBit(4)|charmBit(5)|charmBit(6)|charmBit(7)|charmBit(8)|charmBit(10)|charmBit(12)|charmBit(14)|charmBit(18)|charmBit(19)|charmBit(20)|charmBit(22);
    bool has(unsigned id) const { return (owned&charmBit(id))!=0; }
    bool wears(unsigned id) const { return (equipped&charmBit(id))!=0; }
    unsigned used() const { unsigned n=0;for(const auto& d:charmDefinitions)if(wears(d.id))n+=d.cost;return n; }
    bool overcharmed() const { return used()>notches; }
    bool valid() const {
        unsigned largest=0;for(const auto& d:charmDefinitions)if(wears(d.id))largest=std::max(largest,d.cost);
        return !(owned&~mask)&&!(equipped&~owned)&&notches>=3&&notches<=11&&(!overcharmed()||used()-largest<notches);
    }
    bool toggle(unsigned index,unsigned& attempts) {
        if(index>=charmDefinitions.size())return false;
        const auto& d=charmDefinitions[index];const auto bit=charmBit(d.id);
        if(!(owned&bit))return false;
        if(equipped&bit){equipped&=~bit;attempts=0;return true;}
        if(used()>=notches)return false;
        if(used()+d.cost>notches&&++attempts<5)return false;
        equipped|=bit;attempts=0;return true;
    }
    bool buy(unsigned index,unsigned& geo,const char* scene,bool slyRescued=false) {
        if(index>=charmDefinitions.size()||!scene)return false;
        const auto& d=charmDefinitions[index];
        const bool sly=d.id==1||d.id==4,iselda=d.id==2;
        if(sly&&!slyRescued)return false;
        const char* room=sly?"Room_shop":iselda?"Room_mapper":"Room_Charm_Shop";
        if(std::strcmp(scene,room)||!d.price||has(d.id)||geo<d.price)return false;
        geo-=d.price;owned|=charmBit(d.id);return true;
    }
    bool buyNotch(unsigned& geo,const char* scene) {
        constexpr unsigned prices[]={120,500,900,1400},required[]={5,10,18,25};
        if(!scene||std::strcmp(scene,"Room_Charm_Shop")||notches<3||notches>=7)return false;
        unsigned count=0;for(const auto& d:charmDefinitions)count+=has(d.id);
        const unsigned n=notches-3;if(count<required[n]||geo<prices[n])return false;
        geo-=prices[n];++notches;return true;
    }
    // Only toggles this page owns are changed; later-game modifiers survive.
    void apply(Hero& h) const {
        auto& c=h.charms;
        c.gatheringSwarm=wears(1);c.compass=wears(2);c.grubsong=wears(3);c.stalwart=wears(4);
        c.baldurShell=wears(5);c.fury=wears(6);c.quickFocus=wears(7);c.thorns=wears(12);
        c.steadyBody=wears(14);c.longnail=wears(18);c.shamanStone=wears(19);c.soulCatcher=wears(20);
        c.glowingWomb=wears(22);c.overcharmed=overcharmed();
        if(c.lifebloodHeart!=wears(8)){
            h.vitals.blueHealth=std::max(0,h.vitals.blueHealth+(wears(8)?2:-2));c.lifebloodHeart=wears(8);
        }
    }
};
struct CharmsPage {
    bool open=false;unsigned selected=0,overcharmAttempts=0;
    int inputX=0,inputY=0;
    void navigate(const Input& input) {
        const int x=horizontal(input.x),y=vertical(input.y);
        if(x&&x!=inputX)move(x);
        if(y&&y!=inputY)move(-7*y);
        inputX=x;inputY=y;
    }
    void move(int delta) { selected=unsigned((int(selected)+delta+int(charmDefinitions.size())*2)%int(charmDefinitions.size()));overcharmAttempts=0; }
};
}
