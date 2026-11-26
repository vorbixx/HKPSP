#pragma once
#include "physics.hpp"

namespace hk3ds::shamanquest {
// Steam build 22529139, level76 GameObject1916 / Transform6243.
// Fall14190 / Set Respawns passes the marker name to SetBenchRespawn;
// SetHazardRespawn has its own literal Vector3 parameter.
inline constexpr char scene[]="Crossroads_ShamanTemple";
inline constexpr char marker[]="Death Respawn Marker";
inline constexpr Vec2 checkpoint{47.37788009643555f,8.57796859741211f};
inline constexpr Vec2 hazard{47.44f,8.58f};
inline constexpr bool facingRight=true;
inline constexpr unsigned boneGateOwner=2144,boneGateCollider=9288;
inline constexpr int gateOpenedState=6;
inline constexpr float shamanX=39.27f;
inline constexpr Rect listenRange{32.656f,6.944f,45.884f,7.208f};
inline constexpr char defeatedDialogue[]="@Shaman/SHAMAN_KILLED_BLOCKER_2";
inline constexpr char farewellDialogue[]="@Shaman/SHAMAN_OPEN_GATE";
}
