#pragma once
#include "resting.hpp"
#include "breakable.hpp"
#include "simulation.hpp"
#include "scene_activation.hpp"
#include "trigger_polygon.hpp"
#include <vector>

namespace hk3ds
{
struct RoomDoorStartup
{
    enum : unsigned { Enabled=1, WaitForScene=2, CrossroadsAscent=4 };
    unsigned flags=0;
    float pause=0,normal=0,visited=0,unvisited=0;
};
// Private serialized room observations. Values are supplied by the converter,
// not compiled source scene coordinates or an emulated PlayMaker VM.
struct RoomGate
{
    Rect trigger{};
    Vec2 offset{};
    float delay=0,entryPause=0;
    char name[32]{}, scene[32]{}, entry[32]{};
    // 0: legacy name policy; 1: automatic; 2: authored door interaction.
    unsigned entryKind=0;
    bool unavailable=false;
    // Serialized TransitionPoint / Door Control properties (optional HKD1 bank).
    enum : unsigned { Door=1, Interact=2, FaceRight=4, FaceLeft=8, NoWalk=16, DownInteract=32, JustFade=64, CustomFade=128 };
    unsigned flags=0;
    Vec2 position{},promptPosition{};
    char prompt[32]{};
    RoomDoorStartup startup;
};
// A door entered by Up/Interact (Door Control), not by walking into its trigger.
bool interactiveDoorGate(const RoomGate& gate);
struct RoomCameraLock
{
    Rect trigger{}, limits{};
    unsigned flags=0,owner=0;
};
struct RoomDoor
{
    DoorConfig collision;
    unsigned persistent=0,owner=0;
    std::vector<unsigned> wholeOwners,fadeOwners;
    float fadeSeconds=0;
};
struct RoomCrawler
{
    Vec2 spawn{};float scale=1;unsigned count=2;
    bool initialActive=true,contactActivation=false,disableIfVisitedDirtmouth=false;
    Rect activationTrigger{};
    // Zero disables the viewport shortcut; otherwise aspect - 16/9 >= threshold.
    float ultrawideThreshold=0;
    // Tutorial transform Z and the measured first-activation last SetZ writer.
    float audioInitialDepth=0,audioMinimumDepth=0;
};
struct RoomHazard {
    Rect trigger{};unsigned damage=1,type=2;
    // HKO2 preserves HeroBox.CheckForDamage's source shadowDashHazard bit.
    // The record size stays identical to legacy HKR/HKO1 room banks.
    static constexpr unsigned ShadeDashPassable=1u<<8;
    unsigned hazardType()const{return type&~ShadeDashPassable;}
};
// GO UP has no transition in the crawler FSM; preserve it for other receivers.
struct RoomEnemyMessage {Rect trigger{};unsigned layers=0,event=0;};
struct RoomCheckpoint {Rect trigger{};Vec2 marker{};unsigned flags=0;};
struct RoomReveal
{
    std::vector<Rect> triggers;
    std::vector<unsigned> owners;
    float seconds=0;
};
struct RoomRemask
{
    Rect trigger{};
    std::vector<unsigned> owners;
    float seconds=0;
    bool opaqueInside=true;
    std::vector<Vec2> polygon;
    float startupPause=0,startupFade=0;
    bool startupHeroPositioned=false; // Listens for placement; already-positioned does not send the event.
    std::vector<unsigned> polygonEnds{}; // HKS3: one fade state for a union of box triggers.
    unsigned requiredCharm=0; // HKS4: source equippedCharm_N predicate, sampled on scene entry.
};
bool roomMaskOverlap(Rect body,const RoomRemask& mask);
struct RoomSecret
{
    Rect trigger{};
    unsigned owner=0,saveFlag=0;
    float reloadSeconds=.1f;
    float discoverySeconds=.5f;
};
class RoomConfig
{
public:
    static constexpr unsigned maxRemasks=16;
    bool load(const char* path);
    void supportWater(Hero& hero,const SceneActivationState* activation=nullptr) const;
    std::vector<Rect> water;
    SceneActivationBank activation;
    std::vector<Rect> acidWater;
    Vec2 initial{}, retry{};
    bool initialRight=false,retryRight=false;
    Rect cameraLimits{};
    float deathWait=0,deathFadeWait=.8f,wakeWait=0,lookAhead=0,dampTime=0;
    float entryWait=0,entrySpeed=0;
    float hazardFadeWait=0,hazardWakeWait=0,hazardInvulnerability=0,groundProbe=50;
    Vec2 initialHazard{};
    bool initialHazardRight=false;
    Rect collapseTrigger{},heroHurtbox=bodyBounds({});
    float collapseWait=0;
    unsigned collapseCamera=~0u;
    std::vector<unsigned> collapseShapes;
    float initialLandY=0,initialLandWait=0,initialFlockWait=0,initialGetUpWait=0;
    std::vector<RoomCrawler> crawlers;
    std::vector<RoomEnemyMessage> enemyMessages;
    std::vector<RoomDoor> doors;
    std::vector<RoomCameraLock> locks;
    std::vector<RoomGate> gates;
    std::vector<RestPoint> restPoints;
    StagConfig stag;
    std::vector<RoomHazard> hazards;
    std::vector<TriggerPolygon> hazardShapes; // Optional HKO3 refinement, indexed with hazards.
    std::vector<RoomCheckpoint> checkpoints;
    std::vector<RoomReveal> reveals;
    std::vector<RoomRemask> remasks;
    std::vector<RoomSecret> secrets;
    DoorConfig breakFloor;
    Rect breakFloorRange{};
    float breakFloorWait=0;
    std::vector<unsigned> breakFloorOwners;
};
enum class RoomPhase { Entering, Playing, Dead, Waking, Transition, InitialLand, InitialFlock, InitialGetUp, HazardDeath, HazardWake, DoorArrival };
struct RoomProgress
{
    std::uint32_t broken=0, deaths=0, retries=0, hits=0, kills=0, hazardRespawns=0;
    bool collapsed=false;
};
// A transition is a terminal contract for a room loader. No next scene is
// constructed unless its complete bank and implementation are available.
class RoomFlow
{
    friend struct SimulationStateHash;
public:
    void begin(const RoomConfig& config,Simulation& simulation,const World& world,bool retry=false,bool visitedCrossroads=false);
    // GameManager's completion event is separate from room phase / hero control.
    void finishedEnteringScene();
    void setVisitedCrossroads(bool visited) {visitedCrossroads_=visited;}
    void enteredThrough(const char* gate);
    void groundDoorArrival(Hero& hero,const World& world);
    bool beginDoorArrival(Simulation& simulation,const AnimationBank& bank,float delayBeforeEnter=0);
    void groundHero(Hero& hero,const World& world) const;
    void groundArrival(Hero& hero,const World& world);
    // Pass the room bank so source collapse IDs can be deactivated in place.
    // Supply the latched simulation delta when called after updateFrame.
    // Standalone controls may omit it and use seconds * hero.scale.
    // Arena locks suppress gate admission while retaining death and camera updates.
    void update(float seconds,Simulation& simulation,const World& world,TerrainBank* terrain=nullptr,float scaledSeconds=-1,bool allowTransitions=true,const AnimationBank* heroBank=nullptr);
    bool retryRequested=false;
    RoomPhase phase=RoomPhase::Entering;
    const RoomGate* transition=nullptr;
    RoomProgress progress;
    Vec2 camera{},hazardMarker{};
    Vec2 renderCamera(double pendingSeconds) const
    {return cameraHistory_.sample(camera,{float(pendingSeconds/tuning::controlStep)});}
    AnimationPlayer presentation;
    bool hazardRight=false;
    float deathFade() const;
    const RoomGate* doorPrompt() const {return prompt_;}
    void setActivation(const SceneActivationState* state) {activation_=state;}
    void setEncounterCamera(bool enabled,Rect limits) {encounterCamera_=enabled;encounterLimits_=limits;}
    void setCameraLockFilter(const void* context,bool (*enabled)(const void*,unsigned)) {
        cameraLockContext_=context;cameraLockEnabled_=enabled;
    }
    void setGateFilter(const void* context,bool (*enabled)(const void*,unsigned)) {
        gateContext_=context;gateEnabled_=enabled;
    }
    bool gateEnabled(unsigned index) const {
        return (!activation_||activation_->enabled(SceneActivationTarget::Gate,index))&&
               (!gateEnabled_||gateEnabled_(gateContext_,index));
    }
    void advanceTransition(float scaledSeconds);
    // One suspended-scene tick: sample scale, then resume GameClock owners.
    void advanceTransition(float unscaledSeconds,Simulation& simulation);
    bool transitionReady() const;
    float departureEntryPause() const {return departureEntryPause_;}
    float transitionFade() const;
    void fadeIn(float seconds=.5f) {holdArrivalBlanker_=false;arrivalFade_=seconds;arrivalDelay_=.1f;arrivalFadeDelay_=true;}
    void advanceArrivalFade(float seconds);
    bool crumbling() const {return crumbling_;}
private:
    PresentationPosition cameraHistory_;
    const RoomConfig* config_=nullptr;
    const SceneActivationState* activation_=nullptr;
    const void* gateContext_=nullptr;
    bool (*gateEnabled_)(const void*,unsigned)=nullptr;
    bool encounterCamera_=false;
    Rect encounterLimits_{};
    const void* cameraLockContext_=nullptr;
    bool (*cameraLockEnabled_)(const void*,unsigned)=nullptr;
    const RoomGate* entryGate_=nullptr;
    enum class DoorArrivalStage { EndOfFrame, EntryPause, GateDelay, FadeWait, Walk };
    DoorArrivalStage doorArrivalStage_=DoorArrivalStage::EndOfFrame;
    float doorArrivalTime_=0,doorEntryPause_=0,departureEntryPause_=0;
    void updateDoorArrival(float scaledSeconds,Simulation& simulation,const AnimationBank& bank);
    const RoomGate* prompt_=nullptr;
    enum class DoorCancelPhase { None, NextFrame, Idle };
    struct DoorCancel {
        std::uint64_t physicsTick=0;
        DoorCancelPhase phase=DoorCancelPhase::None;
    };
    std::vector<DoorCancel> doorCancels_;
    enum class DoorStartupPhase { Disabled, WaitingScene, Pause, Wait, Ready };
    struct DoorStartupState {
        DoorStartupPhase phase=DoorStartupPhase::Disabled;
        float timer=0,wait=0;
    };
    void startDoor(unsigned index);
    void enterDoorInit(unsigned index);
    void updateDoorStartup(float scaled,Simulation& simulation);
    std::vector<DoorStartupState> doorStartup_;
    bool finishedEnteringScene_=false,visitedCrossroads_=false;
    bool upHeld_=false,downHeld_=false;
    float transitionTime_=0,arrivalFade_=0;
    float arrivalDelay_=0;
    bool arrivalFadeDelay_=false;
    bool holdArrivalBlanker_=false;
    float departureFadeTime_=0;
    bool departureFadeDelay_=true;
    float timer_=0,look_=0,collapseTimer_=0;
    float respawnFade_=0;
    unsigned deathStage_=0;
    bool wakeYield_=false,crumbling_=false,hardLandingPending_=false,hazardYield_=false;
    std::vector<bool> checkpointInside_,checkpointFired_;
};
bool roomOverlap(Rect a,Rect b);
}
