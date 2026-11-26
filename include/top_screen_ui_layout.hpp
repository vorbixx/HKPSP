#pragma once
#include <cmath>

namespace hk3ds::topui {
// User-selected larger vitals for readability on the New 3DS LCD.
// Preserve the source art proportions and keep the enlarged HUD in the corner.
inline constexpr float vitalsScale=1.12f;
inline constexpr float vitalsX=-26.f, vitalsY=-28.f;
// Enlarge the existing 320x240 modal layouts uniformly, centered horizontally
// and anchored to the bottom so instructions remain inside the 400x240 LCD.
inline constexpr float panelScale=1.1f;
inline constexpr float panelX=24.f, panelY=-24.f;
// Lore can use the top LCD's wider span while retaining the shared body size.
inline constexpr float loreBodyWidth=340.f;
static_assert(panelX+(160.f-loreBodyWidth/2)*panelScale>=8.f &&
              panelX+(160.f+loreBodyWidth/2)*panelScale<=392.f);
// Round in physical LCD coordinates, then return to the active view's units.
// Rounding before the enlarged view leaves text on fractional screen pixels.
inline float snapPixel(float coordinate,float scale,float translation)
{
    return scale==0?coordinate:(std::round(coordinate*scale+translation)-translation)/scale;
}
}
