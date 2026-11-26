#pragma once
#include <cstdint>

namespace hk3ds::cameraColor
{
// Tutorial_01 camera, build 22529139. Fit to the exported RGB curve texture;
// RGB byte constants are packed for PICA, not sampled scene colours.
// Approximation: one constant seed and four bounded TEV stages replace the
// nonlinear 256-entry LUT; the sixth stage applies saturation.
// Endpoints stay black/white; see tools/compare_camera_color.py for the error
// against the original and the rounding model used to choose these constants.
constexpr std::uint32_t curveA = 0xff995227; // (39,82,153)
constexpr std::uint32_t curveB = 0xff008126; // (38,129,0)
constexpr std::uint32_t curveC = 0xff673cdd; // (221,60,103)
constexpr std::uint32_t half = 0xff808080;
constexpr std::uint32_t saturation = 0xffebebeb; // original .92 -> 235/255
struct Config {
    std::uint32_t a=curveA,b=curveB,c=curveC,sat=saturation;
    std::uint32_t preA=0xff404040,preB=0xff404040,preC=0xff808080;
    bool twoPass=false;
};
}
