#pragma once

#include <array>

namespace hk3ds {

// Darkness profiles are authored by the room lighting data.  Lanterns only
// affect positive (deep) darkness; normal and locally bright regions keep the
// supplied profile unchanged.
struct WorldDarkness {
    // FSM20931 Lantern Up has a four-second ScaleTo (the separate entrance
    // glow and its .5-second delay still belong to the presentation adapter).
    static constexpr float lanternSeconds = 4.0f;

    static std::array<float,2> scales(int level,bool lantern,float normal,float damage) {
        if(level>0 && lantern) return {3.0f,2.2f};
        // Vignette FSM20931 Dark 2 / Dark 2 2 uses the same .8 scale
        // for idle and damage until hasLantern selects Lantern 2.
        if(level>=2) return {.8f,.8f};
        return {normal,damage};
    }
};

}
