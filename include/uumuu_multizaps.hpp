#pragma once
#include "uumuu_attacks.hpp"
#include <array>

namespace hk3ds {
enum class UumuuMultizapPhase {Inactive,Delay,Antic,Active,Ending,EndPause};
struct UumuuMultizapEvents {bool hide=false,antic=false,strike=false,end=false,deactivate=false;};
struct UumuuMultizapActor {
    UumuuMultizapPhase phase=UumuuMultizapPhase::Inactive;
    float timer=0,scaleX=1,rotation=0;
    UumuuMultizapEvents events{};
};

// Fixed original arena patterns, not the separate pooled chase-zap prefab.
// A room adapter supplies original animation/particle rendering and reports
// Zap End completion. There is no substitute animation-completion timeout.
class UumuuMultizaps {
public:
    static constexpr unsigned capacity=46,patternCount=2;
    bool activatePattern(unsigned,EnemyRandom);
    unsigned activateRandomPattern(EnemyRandom);
    bool onAttackEvents(const UumuuAttackEvents&,EnemyRandom);
    void fixedUpdate(float);
    // World-space original HeroBox supplied by the owning collision adapter.
    void contactHero(Hero&,Rect heroBox)const;
    bool endAnimationFinished(unsigned);
    void deactivate();
    static Vec2 position(unsigned);
    // Drain after every source frame before advancing another one.
    UumuuMultizapEvents takeEvents(unsigned i){
        if(i>=capacity)return {};
        auto result=actors[i].events;actors[i].events={};return result;
    }
    std::array<UumuuMultizapActor,capacity> actors{};
};
}
