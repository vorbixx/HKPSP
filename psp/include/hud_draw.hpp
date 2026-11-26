#pragma once
#include "draw_list.hpp"
#include "hud.hpp"

namespace hkpsp {
inline constexpr float hudPixelsPerUnit=52.694f*.25f;
// Stable-v1 landmark estimate (task brief), including the extra native PSP row.
inline constexpr Point hudAnchor{54.15f,29.7f};
class HudArt {
    std::array<const hk3ds::Clip*,25> clips{};
public:
    hk3ds::AnimationBank bank;
    FontBank fonts;
    unsigned firstPage=0,fontPage=0;
    HudArt(hk3ds::AnimationBank art,FontBank text,unsigned first,unsigned font);
    const hk3ds::SpriteFrame& frame(unsigned clip,float seconds=0,unsigned index=~0u)const;
};
// Rendering only: observations and all animation clocks belong to shared HudPresentation.
// A cache belongs to one immutable HudArt/font bank and one HUD instance.
struct HudTextCache {
    // Immutable art/texels, one HUD instance. Keep full subpixel sample geometry:
    // integer bounds alone do not determine bilinear alpha at the pixel centres.
    struct Liquid {
        struct Span {std::uint16_t x0,x1,y;std::uint8_t alpha;};
        std::vector<Span> spans;
        std::array<float,16> geometry{};
        std::array<unsigned,5> sampling{};
        const hk3ds::SpriteFrame* frame=nullptr;
        const std::vector<Texture>* textures=nullptr;
        float soul=-1;
        unsigned rebuilds=0;
    } liquid;
    struct Entry {
        std::string text;
        Point center{};
        float alpha=-1;
        unsigned fontPage=~0u;
        std::vector<Quad> quads;
    };
    std::array<Entry,2> entries;
};
void appendHud(DrawList& list,const HudArt& art,const hk3ds::HudPresentation& hud,float seconds,
               HudTextCache* textCache=nullptr,bool large=false);
}
