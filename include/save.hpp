#pragma once
#include <cstdint>
#include "conditional_save.hpp"
#include "resting.hpp"
#include "collectibles.hpp"
#include "charms.hpp"
#include "enemy_life_save.hpp"
#include "saved_player_data.hpp"
#include "cartography.hpp"
#include "watcher_progress.hpp"
#include "uumuu_progress.hpp"
#include "scene_bool_save.hpp"
#include "save_slots.hpp"
namespace hk3ds
{
struct TutorialSave
{
    // Main v1 uses bits 0..5; main v2 adds Fury. v3 also stores the Shade.
    // Branch v2 is distinguished by its 56-byte layout (main v2 is 36 bytes).
    static constexpr std::uint32_t secretFlags=2048|4096;
    static constexpr std::uint32_t gruzMotherFlag=8192,falseKnightFlag=16384,cityCrestFlag=32768,falseKnightFloorFlag=65536;
    static constexpr std::uint32_t mawlekFlag=524288;
    static constexpr std::uint32_t hornetFlag=1048576;
    static constexpr std::uint32_t falseKnightFirstPlopFlag=2097152,dungDefenderFlag=4194304;
    static_assert((falseKnightFirstPlopFlag & dungDefenderFlag)==0, "Boss save flags must be distinct");
    static constexpr std::uint32_t bossFlags=gruzMotherFlag|falseKnightFlag|cityCrestFlag|falseKnightFloorFlag|mawlekFlag|hornetFlag|falseKnightFirstPlopFlag|dungDefenderFlag;
    static constexpr std::uint32_t vengefulSpiritFlag=131072,mothwingCloakFlag=262144;
    static constexpr std::uint32_t abilityFlags=vengefulSpiritFlag|mothwingCloakFlag;
    static constexpr std::uint32_t legacyFlags=63,validFlags=2047|secretFlags|bossFlags|abilityFlags;
    std::uint32_t broken=0,flags=0,geo=0,crawlerKills=0,buzzerKills=0;
    std::uint32_t shadePresent=0,shadeGeo=0;
    float shadeX=0,shadeY=0;
    std::uint32_t shadeHealth=0;
    ConditionalSave conditional{};
    char scene[32]="Tutorial_01",gate[32]{};
    RestSave resting{};
    CollectibleProgress collectibles{};
    CharmInventory charms{};
    // Session handoff only until the save owner composes this into the disk schema.
    EnemyLifeSnapshot enemyDeaths{};
    SavedPlayerData playerData{};
    CartographySave cartography{};
    WatcherProgress watcherProgress{};
    UumuuProgress uumuuProgress{};
    SceneBoolSave sceneBools{};
    bool load(const char* path);
    bool load(const char* path,SaveStorage& storage);
    // Stateless: reads the file to find the slot to overwrite.
    bool write(const char* path) const;
    // Session writer: one in-place slot write while the cursor is known.
    bool write(const char* path,SaveCursor& cursor,SaveStorage& storage) const;
    static bool clear(const char* path);
};
}
