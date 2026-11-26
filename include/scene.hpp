#pragma once
#include "room.hpp"
#include "enemy_rewards.hpp"
#include "scene_battle.hpp"
namespace hk3ds {
struct SceneSpawn {
    std::uint32_t owner=0;
    char prefab[64]{};
    Vec2 position{},scale{1,1};
    bool active=false;
    bool saveActive=true;
    float goamRotation=0;
    bool goamRotationSet=false;
    // Optional source placement overrides; older room banks use species defaults.
    EnemyRewardConfig reward{};
    bool hasReward=false;
};
struct SceneBossRoom {
    struct Gate {unsigned collider=0;Rect bounds{};std::vector<unsigned> owners;};
    bool authored=false;
    Rect arena{},trigger{};
    std::vector<Gate> gates;
    std::vector<unsigned> floorColliders,floorOwners,brokenFloorOwners;
};
// One room's small metadata bank. Art/terrain are owned by platform adapters.
class SceneBank {
public:
    bool load(const char* path);
    // A saved checkpoint may be authored by a progression gate rather than a bench.
    bool enter(const char* gate,const Vec2* checkpoint=nullptr);
    RoomConfig rules;
    std::vector<SceneSpawn> enemies;
    SceneBossRoom bossRoom;
    SceneBattleData battle;
    bool defaultAmbient=false;
    unsigned ambientZone=0;
};
bool validSceneIdentifier(const char* value, bool allowEmpty=false);
bool validGateIdentifier(const char* value, bool allowEmpty=false);
bool startupSceneMetadataAvailable(const char* root,const char* scene);
bool scenePackageAvailable(const char* root,const char* scene,const char* roomPrefix="room-resident-");
void closeUnavailableSceneGates(RoomConfig& rules,const char* root,const char* roomPrefix="room-resident-");
void addUnavailableGateBarriers(const RoomConfig& rules,TerrainBank& terrain);
}
