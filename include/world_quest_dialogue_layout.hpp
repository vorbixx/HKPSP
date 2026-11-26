#pragma once

namespace hk3ds::questdialogue {
// Native 400x240 projection measured from the grounded, fully revealed
// original Bretta first-page capture. Reuse the original dialogue artwork.
inline constexpr float left=82.f, baseline=44.f, scale=.6f, lineHeight=10.f;
inline constexpr float center=200.f, top=25.f, bottom=97.f;
inline constexpr float backboardY=63.f, backboardScale=.9f;
inline constexpr float topScale=.825f, bottomScale=1.2f, markerScale=1.2f;
inline constexpr float markerX=201.f, markerY=99.f;
}

namespace hk3ds::tolldialogue {
// The same original font/art, laid out for readable 400x240 toll information.
inline constexpr float left=40.f,baseline=62.f,scale=1.1f,lineHeight=18.f;
inline constexpr float center=200.f,top=40.f,bottom=157.f;
inline constexpr float backboardY=98.f,backboardScale=1.55f;
inline constexpr float topScale=1.2f,bottomScale=1.8f,markerScale=1.2f;
inline constexpr float markerX=350.f,markerY=139.f,choicesY=130.f;
}
