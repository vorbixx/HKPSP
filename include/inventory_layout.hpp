#pragma once
#include "hud_text_layout.hpp"
#include "ui.hpp"

namespace hk3ds::inventorylayout {
inline constexpr float width=320, height=240, center=width/2;
inline constexpr float textScale=ui::scale(ui::Size::Body);
inline constexpr ui::Rect charmNameBounds{104,20,272,28};
inline constexpr ui::Rect charmDescriptionBounds{24,104,352,120};
static_assert(hudtext::scaleIndex(textScale)<hudtext::scaleCount,
              "Inventory text needs a baked LCD glyph scale");
// Reflow the former 400px columns; this changes positions, never glyph scale.
constexpr float x(float original) {return original*width/400;}
inline constexpr float geoIconX=24,geoValueCenter=79,healthIconX=153,healthValueCenter=188;
inline constexpr float soulIconX=239,soulValueCenter=281;
constexpr float tabCenter(unsigned tab) {return x(78+122*tab);}
constexpr float itemX(unsigned slot) {return x(60+70*(slot%5));}
constexpr float itemY(unsigned slot) {return 92+32*(slot/5);}
constexpr float charmX(unsigned slot,bool embedded) {
    return embedded?x(51+(slot%7)*49):51+(slot%7)*49;
}
constexpr float charmY(unsigned slot,bool embedded) {
    return (embedded?114:74)+32*(slot/7);
}
constexpr float notchY(bool embedded) {return embedded?80:39;}
constexpr float notchX(unsigned notch,bool embedded) {
    return embedded?x(180+notch*15):180+notch*15;
}
}
