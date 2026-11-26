#pragma once
#include "terrain.hpp"
#include "combat.hpp"
#include "scene_bool_save.hpp"
namespace hk3ds {
class DecorEffects;
// One bounded interaction per source wall. Physics owns the intact polygons;
// persistence never shares tutorial door bits or enemy health.
class SceneWalls {
    friend struct SimulationStateHash;
public:
    void bind(TerrainBank&,SceneBoolSave&,const SavedPlayerData&,DecorEffects* effects=nullptr);
    void restore(const SavedPlayerData&);
    void enter(float heroX);
    void clear(){terrain_=nullptr;saved_=nullptr;effects_=nullptr;states_.clear();changed_=false;playerData_=0;cues_={};}
    void resetPose();
    void update(float,Hero&,const NailBank*);
    bool hit(unsigned,AttackType);
    bool wallBreaker(unsigned);
    void spell(Rect);
    void applyCollision();
    float alpha(unsigned owner) const;
    template<class Emit> void visitAlphaOwners(Emit emit)const{
        if(terrain_)for(const auto& wall:terrain_->walls){emit(wall.root);for(const auto owner:wall.children)emit(owner);}
    }
    bool hidesLock(unsigned owner) const;
    bool takeChanged(){const bool result=changed_;changed_=false;return result;}
    unsigned takePlayerData(){const auto result=playerData_;playerData_=0;return result;}
    const std::array<unsigned,2>& cues() const{return cues_;}
    void bindVisuals(RoomVisualBank&);
    void presentVisuals(RoomVisualBank&,PresentationTime);
private:
    struct State {
        unsigned remaining=0,serial=0;
        bool broken=false,deactivated=false,heroInRange=false;
        float cooldown=0,delay=0,fade=0;
        Vec2 offset{};
        PresentationPosition presentation;
        struct Draw {unsigned index=0;float x=0,y=0;};
        std::vector<Draw> draws;
    };
    TerrainBank* terrain_=nullptr;
    SceneBoolSave* saved_=nullptr;
    DecorEffects* effects_=nullptr;
    std::array<unsigned,2> cues_{};
    std::vector<State> states_;
    unsigned playerData_=0;
    bool changed_=false;
    bool breakWall(unsigned);
    void move(unsigned,Vec2);
};
}
