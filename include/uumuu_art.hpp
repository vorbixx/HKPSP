#pragma once
#include "actor_texture_budget.hpp"

namespace hk3ds {
// Deterministic scale-1 packing of the copied build22529139 sprite rectangles.
// Converters verify these layouts. These are allocation costs, not an increase
// to the room budget or a claim that the banks fit every hardware mode.
inline constexpr ActorTexturePage uumuuBodyPages[]={
    {1024,1024},{1024,1024},{1024,1024},{1024,1024},{1024,1024},{512,512}
};
inline constexpr ActorTexturePage uumuuQuirrelPages[]={
    {1024,1024},{1024,1024},{256,256}
};
inline constexpr ActorTexturePage uumuuEffectsPages[]={{1024,1024},{128,128}};
inline constexpr std::size_t uumuuEffectsPageCount=2;
inline constexpr std::size_t uumuuEffectsTextureBytes=2097152+32768;
inline constexpr ActorTexturePage uumuuGatePages[]={{1024,1024}};
inline constexpr std::size_t uumuuGatePageCount=1;
inline constexpr std::size_t uumuuGateTextureBytes=2097152;
inline constexpr std::size_t uumuuBodyPageCount=sizeof(uumuuBodyPages)/sizeof(*uumuuBodyPages);
inline constexpr std::size_t uumuuQuirrelPageCount=sizeof(uumuuQuirrelPages)/sizeof(*uumuuQuirrelPages);
inline constexpr std::size_t uumuuBodyTextureBytes=11010048;
inline constexpr std::size_t uumuuQuirrelTextureBytes=4325376;
inline constexpr std::size_t uumuuActorTextureBytes=uumuuBodyTextureBytes+uumuuQuirrelTextureBytes+uumuuGateTextureBytes+uumuuEffectsTextureBytes;
static_assert([]{std::size_t bytes=0;return actorTextureBytes(uumuuEffectsPages,uumuuEffectsPageCount,bytes)&&
    bytes==uumuuEffectsTextureBytes;}(),"Uumuu effects allocation plan changed");
static_assert([]{std::size_t bytes=0;return actorTextureBytes(uumuuGatePages,uumuuGatePageCount,bytes)&&
    bytes==uumuuGateTextureBytes;}(),"Archive gate allocation plan changed");
static_assert([]{std::size_t bytes=0;return actorTextureBytes(uumuuBodyPages,uumuuBodyPageCount,bytes)&&
    bytes==uumuuBodyTextureBytes;}(),"Uumuu body allocation plan changed");
static_assert([]{std::size_t bytes=0;return actorTextureBytes(uumuuQuirrelPages,uumuuQuirrelPageCount,bytes)&&
    bytes==uumuuQuirrelTextureBytes;}(),"Quirrel allocation plan changed");
}
