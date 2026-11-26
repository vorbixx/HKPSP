#pragma once
#include "physics.hpp"
#include "presentation.hpp"
#include "scene_bool_save.hpp"
#include "room_masks.hpp"
#include "scene_lever.hpp"
#include <vector>
#include <cstddef>
namespace hk3ds
{
constexpr std::size_t terrainShapeCapacity=256;
constexpr std::size_t terrainVertexCapacity=8192;
struct Hero;
struct TerrainShroom {
    unsigned owner=0,collider=0,attackSerial=0;
    Vec2 center{};
    float radius=0;
    unsigned draw=~0u,initial=0;
    std::array<std::vector<unsigned>,2> frames;
    std::array<float,2> fps{};
    float elapsed=0,bounceElapsed=-1;
};
class NailBank;
class RoomVisualBank;
struct TerrainWall {
    enum Kind : unsigned { Breakable=0, MantisGate=1 };
    Kind kind=Breakable;
    std::array<Vec2,8> nailTrigger{};
    unsigned nailTriggerCount=4;
    float collisionDelay=0,hideDelay=0;
    unsigned root=0,scene=0,object=0,playerData=0,hits=0,facing=0,shape=0;
    Vec2 origin{};
    float depth=0;
    enum EntryPolicy : unsigned { Always=0, DeactivateLeftOf=1 };
    EntryPolicy entryPolicy=Always;
    float entryX=0;
    // break_floor keeps nail and Hero Detector triggers separate from its solid.
    bool oneWay=false,rejectDown=false;
    float hitWait=0; // Zero retains the pre-HKW5 wall policy.
    Rect hitBounds{},heroRange{};
    std::vector<std::array<Vec2,3>> hitTriangles;
    std::vector<unsigned> children,locks;
    // Source contact floors share persistence and visual ownership with walls.
    bool contactFloor=false;
    float contactDelay=0;
    std::vector<unsigned> linkedShapes;
};
struct TerrainLift {
    std::vector<unsigned> shapes,owners,chains;
    float lower=0,upper=0,speed=0,initial=0,target=0,wait=0;
    Vec2 position{},upperControl{},lowerControl{};
    PresentationPosition presentation;
    bool moving=false,scripted=false;
    bool falling=false,cut=false,saveOnLand=false;
    float acceleration=0,velocity=0;
    SceneBoolSave::Entry key{};
    std::vector<float> stops;
    bool goingDown=false;
    std::vector<std::vector<Vec2>> controls{3};
    unsigned attackSerial=0;
    struct Draw {unsigned index=0;float y=0,alpha=0;bool chain=false;};
    std::vector<Draw> draws;
};
struct TerrainLever {
    unsigned owner=0,gate=0,shape=0,count=0,attackSerial=0;
    SceneBoolSave::Entry key{};
    SceneBoolSave::Entry switchKey{};
    unsigned playerDataKey=0;
    Rect range{};
    std::array<Vec2,16> polygon{};
    SceneLeverState state;
    unsigned firstFrames=0,draw=~0u;
    std::vector<unsigned> frames;
    std::vector<unsigned> switchFrames;
    unsigned switchDraw=~0u,switchIdleFrames=0;
    float switchFps=0,switchIdleFps=0;
    bool switchActivated=false;
};
// Private HKT1 static scene snapshot. Source geometry is never compiled in.
class TerrainBank
{
    friend struct SimulationStateHash;
public:
    bool load(const char* path);
    World world() const;
    bool setEnabled(unsigned id,bool enabled);
    void fixedMotion(float seconds,Hero&,const NailBank* nails=nullptr);
    void fixedLeverMotion(float seconds,const Hero&,const NailBank* nails=nullptr);
    void bindMotionVisuals(RoomVisualBank&);
    void presentMotion(RoomVisualBank&,PresentationTime);
    void bindMotionSave(SceneBoolSave&,SavedPlayerData* playerData=nullptr);
    void applyLeverCollision();
    float leverAlpha(unsigned owner) const;
    bool takeLeverChanged(){const bool result=leverChanged_;leverChanged_=false;return result;}
    std::vector<TerrainLift> lifts;
    std::vector<TerrainWall> walls;
    std::vector<RoomMask> masks;
    std::vector<TerrainLever> levers;
    std::vector<TerrainShroom> shrooms;
    Vec2 crawlerSpawn{};
    Rect region{};
    PhysicsMaterial actorMaterial{}, corpseMaterial{};
    bool sourceContacts=false;
    std::vector<TerrainShape> shapes;
    std::vector<Vec2> vertices;
private:
    SceneBoolSave* motionSave_=nullptr;
    SavedPlayerData* motionPlayerData_=nullptr;
    bool leverChanged_=false;
};
}
