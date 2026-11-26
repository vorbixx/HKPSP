#pragma once
#include <cstdint>

namespace hk3ds::cameraBloom
{
// Steam22529139 Hidden/FastBloom: threshold .35, intensity .43, Low (1/4),
// two standard seven-tap H/V pairs before ColorCorrectionCurves.
constexpr unsigned width=100,height=60,textureWidth=128,textureHeight=64;
constexpr std::uint32_t threshold=0xff595959;
constexpr float sourceIntensity=.43f;
// Preserve the source intensity; a fitted attenuation hid scene-composition
// differences and reduced bloom energy in every room.
constexpr std::uint32_t intensity=0xff6e6e6e; // round(.43*255)=110
// References render at1200x720 and resize to400x240. Preserve their apparent
// bloom radius rather than expanding it threefold at the port's resolution.
constexpr float radiusScale=1.0f/3;
// Pair adjacent taps using bilinear filtering: .232 + .0855 = .3175.
constexpr float offsets[]={0,1.26929134f,-1.26929134f,3,-3};
constexpr unsigned weights[]={83,81,81,5,5}; // sum255; original .324/.3175/.0205
}
