#pragma once
#include "world_switches.hpp"
#include "terrain.hpp"
#include <array>

namespace hk3ds {
// Geometry stays in the map bank. Only source DestroySelf/DestroyObject events
// disable these bindings; OPEN and the switch wait do not disable collision.
class TollSwitchTerrain {
    friend struct SimulationStateHash;
public:
    bool bind(TerrainBank& terrain,std::array<unsigned,TollSwitch::gateCount> colliders)
    {
        if(!valid(terrain,colliders))return false;
        terrain_=&terrain;colliders_=colliders;
        shapes_=terrain.shapes.data();shapeCount_=terrain.shapes.size();return true;
    }
    bool resetCollision(){
        if(!terrain_||terrain_->shapes.data()!=shapes_||terrain_->shapes.size()!=shapeCount_||!valid(*terrain_,colliders_))return false;
        for(unsigned id:colliders_)terrain_->setEnabled(id,true);
        return true;
    }
    bool apply(const TollSwitchEvents& events)
    {
        if(!terrain_||(events.removeGates&~TollSwitch::allGates)||
           terrain_->shapes.data()!=shapes_||terrain_->shapes.size()!=shapeCount_)return false;
        if(!events.removeGates)return true;
        if(!valid(*terrain_,colliders_))return false;
        // The loader must rebind on every room entry. Validate both shapes
        // before mutation, including when the terrain buffers were replaced.
        for(unsigned i=0;i<TollSwitch::gateCount;++i)
            if(events.removeGates&(1u<<i))terrain_->setEnabled(colliders_[i],false);
        return true;
    }
private:
    static bool valid(const TerrainBank& terrain,const std::array<unsigned,TollSwitch::gateCount>& colliders)
    {
        if(!colliders[0]||!colliders[1]||colliders[0]==colliders[1])return false;
        std::array<unsigned,TollSwitch::gateCount> found{};
        for(const auto& shape:terrain.shapes)for(unsigned i=0;i<TollSwitch::gateCount;++i)
            if(shape.id==colliders[i]){
                if(shape.flags&(1u|TerrainShape::queryOnly))return false;
                ++found[i];
            }
        return found[0]==1&&found[1]==1;
    }
    TerrainBank* terrain_=nullptr;
    std::array<unsigned,TollSwitch::gateCount> colliders_{};
    const TerrainShape* shapes_=nullptr;
    std::size_t shapeCount_=0;
};
static_assert(sizeof(TollSwitchTerrain)<=32,"Keep gate terrain bindings bounded");
}
