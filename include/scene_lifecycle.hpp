#pragma once
#include "hero.hpp"

namespace hk3ds {
// Ordinary gate transfers keep live resources. Disk loads and respawns use
// their own reset policy; this payload is never serialized.
struct SceneHeroState {
    Vitals vitals;
    Abilities abilities;
    Charms charms;
    explicit SceneHeroState(const Hero& hero)
        :vitals(hero.vitals),abilities(hero.abilities),charms(hero.charms){}
    void apply(Hero& hero) const {
        hero.vitals=vitals;hero.abilities=abilities;hero.charms=charms;
    }
};
}
