#pragma once
#include <cstdint>

namespace hk3ds {
// PlayerData names are retained so later progression adapters need no translation.
// These late-game fields default off; Tutorial_01 never grants either charm.
struct ConditionalSave {
    enum Flag : std::uint32_t { SporeShroom=1, Grimmchild=2, DreamNail=4,
        TutorialFlameCollected=8, KilledGrimmkinMaster=16 };
    std::uint32_t mrMushroomState=0, flags=0, grimmChildLevel=0;
    std::uint32_t flamesCollected=0, flamesRequired=0, nailSmithUpgrades=0;
    bool valid() const {
        return mrMushroomState<=8&&flags<=31&&grimmChildLevel<=5&&
               flamesCollected<=3&&flamesRequired<=3&&nailSmithUpgrades<=4;
    }
    bool mushroomPresent() const { return mrMushroomState==7; }
    bool flamePresent() const {
        return !(flags&TutorialFlameCollected)&&(flags&Grimmchild)&&
               grimmChildLevel==2&&flamesCollected<flamesRequired;
    }
    bool operator==(const ConditionalSave& s) const {
        return mrMushroomState==s.mrMushroomState&&flags==s.flags&&
            grimmChildLevel==s.grimmChildLevel&&flamesCollected==s.flamesCollected&&
            flamesRequired==s.flamesRequired&&nailSmithUpgrades==s.nailSmithUpgrades;
    }
    bool operator!=(const ConditionalSave& s) const { return !(*this==s); }
};
}
