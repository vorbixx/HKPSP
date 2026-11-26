#pragma once
#include "saved_player_data.hpp"
#include "scene_bool_save.hpp"
#include <array>
#include <cstdint>
#include <vector>

namespace hk3ds {
// Original startup conditions compiled by the scene converter, not a PlayMaker VM.
struct SceneActivationNode {
    enum : std::uint32_t { Constant, Equal, NotEqual, Less, LessEqual, Greater,
        GreaterEqual, And, Or, Not, SceneBool };
    std::uint32_t op=0,a=0,b=0;
};
struct SceneActivationTarget {
    enum : std::uint32_t { Visual, Collider, Gate, Spawn, Hazard, AcidWater };
    std::uint32_t kind=0,id=0,node=0;
};
struct SceneActivationBank {
    std::vector<SceneActivationNode> nodes;
    std::vector<SceneActivationTarget> targets;
    // Missing sidecars preserve older room packages. Malformed sidecars fail.
    bool load(const char* path);
};
class SceneActivationState {
    friend struct SimulationStateHash;
public:
    void reset(const SceneActivationBank&,const SavedPlayerData&,
               const SavedPlayerData::Entry* overrides=nullptr,std::size_t overrideCount=0,
               const SceneBoolSave* sceneBools=nullptr);
    bool enabled(unsigned kind,unsigned id) const;
    template<class Emit> void visitDisabledVisuals(Emit emit)const{
        for(const auto owner:disabled_[SceneActivationTarget::Visual])emit(owner);
    }
private:
    std::array<std::vector<unsigned>,6> disabled_;
};
}
