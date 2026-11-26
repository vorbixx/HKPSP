#pragma once
#include "saved_player_data.hpp"
namespace hk3ds {
// Original IK Control, EnemyDeathEffects and corpse Init own separate events.
struct BrokenVesselProgress {
    bool encountered=false, killed=false, corniferLeft=false;
    static BrokenVesselProgress read(const SavedPlayerData& d) {
        return {d.get("infectedKnightEncountered")!=0,d.get("killedInfectedKnight")!=0,d.get("corn_abyssLeft")!=0};
    }
    bool write(SavedPlayerData& d)const {
        auto next=d;
        if(!next.set("infectedKnightEncountered",encountered)||!next.set("killedInfectedKnight",killed)||!next.set("corn_abyssLeft",corniferLeft))return false;
        d=next;return true;
    }
};
}
