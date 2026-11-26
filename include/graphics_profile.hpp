#pragma once
#include "render_memory.hpp"
#include <array>
#include <cstdint>
#include <limits>
namespace hk3ds {
enum class GraphicsMode : std::uint8_t { Auto, New3ds, Old3ds };
struct GraphicsProfile {
    const char* name;
    const char* roomPrefix;
    std::size_t roomTextureBytes;
    bool postprocessing, lighting, ambientParticles, audio;
    float maxRoomDepth;
    unsigned frameDivisor;
    std::size_t enemyAudioBytes;
    bool retainOptionalActors;
    float intermediateRoomDepth, fallbackRoomDepth; // Offline cutoffs when the wider Old bank exceeds budget.
    unsigned diagnosticTraceDivisor; // Bound observer RAM separately from artwork/audio.
};
// Retained art always uses source texture pixels and a native 400x240 playfield.
// Old omits distant foreground/background layers and repacks retained pixels.
inline constexpr std::array<GraphicsProfile,2> graphicsProfiles{{
    {"New 3DS","room-resident-",roomTextureBudget,true,true,true,true,std::numeric_limits<float>::infinity(),1,enemyAudioBankCapBytes,true,0.f,0.f,1},
    {"Old 3DS","room-resident-old-",oldRoomTextureBudget,false,false,false,true,8.f,2,oldEnemyAudioBankCapBytes,false,2.f,1.f,4}
}};
static_assert(graphicsProfiles[0].diagnosticTraceDivisor==1&&
              graphicsProfiles[1].diagnosticTraceDivisor>0,"Preserve New trace size and valid Old capacities");
constexpr unsigned graphicsProfileIndex(GraphicsMode mode,bool new3ds,bool highMemory) {
    return highMemory&&(mode==GraphicsMode::New3ds||(mode==GraphicsMode::Auto&&new3ds))?0:1;
}
// Old admits native clips in table order; oversized clips remain silent.
constexpr bool audioClipFits(std::size_t bytes,std::size_t resident,std::size_t cap) {
    return resident<=cap&&bytes<=cap-resident;
}
inline const GraphicsProfile* selectedGraphicsProfile=&graphicsProfiles[0];
inline const GraphicsProfile& graphicsProfile(){return *selectedGraphicsProfile;}
inline void selectGraphicsProfile(GraphicsMode mode,bool new3ds,bool highMemory) {
    selectedGraphicsProfile=&graphicsProfiles[graphicsProfileIndex(mode,new3ds,highMemory)];
    activeRoomTextureBudget=graphicsProfile().roomTextureBytes;
}
constexpr const char* graphicsModeName(GraphicsMode mode) {
    return mode==GraphicsMode::Auto?"Auto":mode==GraphicsMode::New3ds?"New 3DS":"Old 3DS";
}
}
