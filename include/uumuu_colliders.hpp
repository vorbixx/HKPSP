#pragma once
#include "combat.hpp"

namespace hk3ds {
// Both source triggers belong to Mega Jellyfish's health owner. Geometry and
// collision layer come from its saved GameObject; parent ownership stays intact.
class UumuuColliders {
    friend struct SimulationStateHash;
public:
    UumuuColliders()=default;
    UumuuColliders(const UumuuColliders&)=delete;
    UumuuColliders& operator=(const UumuuColliders&)=delete;
    ~UumuuColliders(){reset();}
    bool bind(CombatObject&,Vec2 position);
    void reset();
    bool sync(Vec2 position,bool enabled);
    void contactHero(Hero&,Rect heroBody)const;
private:
    CombatObject* object_=nullptr;
    CombatCircle circle_{};
    Vec2 position_{};
    Rect previousBounds_{};
    unsigned previousCount_=1,previousLayer_=0;
    bool previousEnabled_=false;
};
}
