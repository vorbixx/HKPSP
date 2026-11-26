#pragma once
#include <algorithm>

namespace hkpsp::inventorylayout {
// Reflow include/inventory_layout.hpp and src/inventory_renderer.cpp into one
// 480x272 screen. Retain pixel density rather than stretching the 320px screen.
struct Rect {float x,y,w,h;};
inline constexpr Rect screen{0,0,480,272},detail{310,46,154,202};
inline constexpr Rect heading{16,10,448,24},footer{16,250,448,20};
inline constexpr Rect geo{242,44,24,24},health{20,44,28,36},soul{152,44,36,36};
inline constexpr Rect equipped{16,44,280,32},notches{16,80,280,12};
constexpr Rect item(unsigned slot) {
    return slot==0?Rect{22,96,40,88}:Rect{86+40.f*((slot-1)%5),96+40.f*((slot-1)/5),32,32};
}
constexpr Rect charm(unsigned slot) {return {16+40.f*(slot%7),106+40.f*(slot/7),38,38};}
constexpr Rect equippedSlot(unsigned slot) {return {16+24.f*slot,44,22,28};}
constexpr Rect notch(unsigned slot) {return {16+15.f*slot,80,10,10};}
constexpr bool inside(Rect a,Rect b=screen) {
    return a.w>0&&a.h>0&&a.x>=b.x&&a.y>=b.y&&a.x+a.w<=b.x+b.w&&a.y+a.h<=b.y+b.h;
}
constexpr bool overlaps(Rect a,Rect b) {
    return a.x<b.x+b.w&&b.x<a.x+a.w&&a.y<b.y+b.h&&b.y<a.y+a.h;
}
inline float fit(float width,float height,Rect bounds) {
    return std::min({1.f,bounds.w/width,bounds.h/height});
}
}
