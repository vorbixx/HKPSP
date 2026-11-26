#pragma once
#include <cmath>
#include "tutorial_prompt_glyphs.hpp"

namespace hk3ds::hudtext {
inline constexpr unsigned scaleCount=sizeof(promptdata::lcdScales)/sizeof(float);
// Glyphs are rasterized at these native LCD sizes; arbitrary scales have no atlas.
constexpr unsigned scaleIndex(float scale) {
    for(unsigned i=0;i<scaleCount;++i){
        const float difference=scale-promptdata::lcdScales[i];
        if(difference>=-.0001f&&difference<=.0001f)return i;
    }
    return scaleCount;
}
inline unsigned glyphScaleIndex(float scale) {
    if(!std::isfinite(scale)||scale<=0)return scaleCount;
    unsigned nearest=0;
    for(unsigned i=1;i<scaleCount;++i)
        if(std::abs(promptdata::lcdScales[i]-scale)<std::abs(promptdata::lcdScales[nearest]-scale))nearest=i;
    return nearest;
}
inline constexpr float regionTitleScale=1.15f;
inline constexpr float regionTitleSpacing=1.2511921f;
// Source small capitals: reuse the resident 16px Trajan atlas at native size.
// Land0 reference bounds are x237..367/y209..222 on the 400x240 viewport.
inline constexpr float bossTitleScale=1.f/1.875f;
inline constexpr float bossTitleX=302,bossTitleBaseline=221;
inline float textWidth(const char* text,float scale,float spacing=0) {
    float width=0;
    bool first=true;
    for(const char* letter=text;*letter;++letter)if(*letter>=32&&*letter<=126){
        if(!first)width+=spacing;
        width+=promptdata::glyphAdvance[*letter-32]*scale;
        first=false;
    }
    return width;
}
static_assert(scaleIndex(regionTitleScale)<scaleCount,"Region title needs a baked LCD glyph scale");
// Station prompts and destination names use the largest resident LCD glyphs.
inline constexpr float stationPromptScale=1.15f;
inline constexpr float stationMenuScale=1.15f;
static_assert(scaleIndex(stationPromptScale)<scaleCount,"Station prompt needs native LCD glyphs");
static_assert(scaleIndex(stationMenuScale)<scaleCount,"Station names need native LCD glyphs");
inline constexpr float restingPromptScale=1.15f;
static_assert(scaleIndex(restingPromptScale)<scaleCount,"Resting prompt needs a baked LCD glyph scale");
}
