#pragma once

#include <array>
#include <cstdint>
#include "physics.hpp"

namespace hk3ds::uumuu_gates_data {
struct Gate { std::uint32_t owner, collider, layer; Rect bounds; bool startClosed; };
inline constexpr std::array<Gate, 3> gates{{
    {3u, 77u, 8u, {50.4679696f, 134.225839f, 55.6534232f, 135.240668f}, false},
    {6u, 79u, 8u, {71.0029103f, 104.956576f, 72.1206706f, 110.142027f}, true},
    {13u, 80u, 8u, {33.995847f, 104.956576f, 34.9976833f, 110.142027f}, true},
}};
inline constexpr const char* closeClip = "Close";
inline constexpr const char* closedClip = "Closed";
inline constexpr const char* openClip = "Open";
inline constexpr const char* openedClip = "Opened";
inline constexpr float openDuration = 7.0f/12.0f;
struct Visual { Vec2 position, scale; float rotation; };
inline constexpr std::array<Visual, 3> visuals{{
    {{53.0699997f, 134.619995f}, {0.800000012f, 0.919999599f}, 1.57079405f},
    {{71.5f, 107.540001f}, {0.800000012f, 0.919999599f}, 0.0f},
    {{34.3899994f, 107.540001f}, {0.800000012f, 0.919999599f}, 0.0f},
}};
} // namespace hk3ds::uumuu_gates_data
