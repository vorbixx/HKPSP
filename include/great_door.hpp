#pragma once
#include "combat.hpp"
#include "terrain.hpp"
#include <array>
namespace hk3ds {
struct GreatDoorTransition {
    char scene[48]{}, gate[48]{};
    float entryDelay=0;
};
struct GreatDoorConfig {
    unsigned terrainId=0, layer=0;
    Rect bounds{};
    std::array<unsigned,3> thresholds{};
    float hitWait=0;
    GreatDoorTransition transition{};
    bool load(const char* path);
};
enum class GreatDoorState { Pause, Idle, HitWait, Break, ExtraFrame, Move, Activated };
// The fresh Break branch preserves the collider: it fades out and automatically
// transitions after two NextFrameEvent actions. Only persisted Activate disables it.
class GreatDoor {
    friend struct SimulationStateHash;
public:
    bool bind(const GreatDoorConfig&,TerrainBank&,CombatObject&,bool persistedActivated=false);
    void update(float scaledDt); // Exactly once per rendered frame; NextFrame is unscaled.
    bool nail(const NailHit&);
    unsigned hits() const { return hits_; }
    unsigned stage() const { return stage_; }
    const std::array<unsigned,3>& thresholds() const { return config_.thresholds; }
    bool activated() const { return activated_; }
    bool visible() const { return visible_; }
    // FSM Tk2dSpriteSetId changes only at Break 1/2. Final Break keeps v03
    // through the transition; the persisted Activate branch hides the renderer.
    const char* spriteName() const { return stage_==0?"door_v01":stage_==1?"door_v02":"door_v03"; }
    bool transitionRequested() const { return state_==GreatDoorState::Move; }
    const GreatDoorTransition& transition() const { return config_.transition; }
    GreatDoorState state() const { return state_; }
    // Presentation can consume counters without mutating the gameplay state.
    unsigned hitEffects=0, crackEffects=0;
private:
    static void receive(void*,const NailHit&,HitOutcome);
    GreatDoorConfig config_{};
    TerrainBank* terrain_=nullptr;
    CombatObject* object_=nullptr;
    GreatDoorState state_=GreatDoorState::Pause;
    unsigned hits_=0,stage_=0;
    float wait_=0;
    bool activated_=false,visible_=true;
};
}
