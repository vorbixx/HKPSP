#pragma once
#include "diagnostic_game_flags.hpp"
#include "graphics_profile.hpp"

namespace hk3ds {
inline bool staticDecorEnabled() {
    return graphicsProfile().roomTextureBytes==roomTextureBudget&&
        gameFlagEnabled("sdmc:/3ds/HK3DS/static-decor.flag",gameFlagStaticDecor,shippedStaticDecor);
}
}
