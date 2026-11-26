#pragma once
#include "build_config.hpp"
#include <cstdio>

#ifndef HK3DS_DIAGNOSTIC_GAME_FLAGS
#define HK3DS_DIAGNOSTIC_GAME_FLAGS -1
#endif
#ifndef HK3DS_DIAGNOSTIC_PERF_ROOM
#define HK3DS_DIAGNOSTIC_PERF_ROOM -1
#endif

namespace hk3ds {
// Shipped benchmark/replay configuration. Keep these values in the runtime
// contract so benchmark fixtures and tests cannot drift when a default moves.
inline constexpr int shippedBenchmarkGameFlags=242;
inline constexpr int benchmarkUseSdDefaults=-1;
inline constexpr unsigned gameFlagFogMesh=1u;
inline constexpr unsigned gameFlagBlurTargetSmall=2u;
inline constexpr unsigned gameFlagBlurTargetHalf=4u;
inline constexpr unsigned gameFlagBlendGrabShared=8u;
inline constexpr unsigned gameFlagEffectsTargetCompact=16u;
inline constexpr unsigned gameFlagHotRoomVram=32u;
inline constexpr unsigned gameFlagStaticDecor=64u;
inline constexpr unsigned gameFlagBackgroundCache=128u;
inline constexpr unsigned gameFlagHalfLight=256u;
inline constexpr bool shippedFogMesh=false;
inline constexpr bool shippedBlurTargetSmall=true;
inline constexpr bool shippedBlurTargetHalf=false;
inline constexpr bool shippedBlendGrabShared=false;
inline constexpr bool shippedEffectsTargetCompact=true;
inline constexpr bool shippedHotRoomVram=true;
inline constexpr bool shippedStaticDecor=true;
inline constexpr bool shippedBackgroundCache=true;
inline constexpr bool shippedHalfLight=true;
// -1 preserves SD selection. A diagnostic A/B forces all bits, including off.
static constexpr int diagnosticGameFlags=diagnosticsEnabled?HK3DS_DIAGNOSTIC_GAME_FLAGS:-1;
static constexpr int diagnosticPerformanceRoom=diagnosticsEnabled?HK3DS_DIAGNOSTIC_PERF_ROOM:-1;
// 0..2 pin the camera; 3..5 move in the same rooms. 6/7 select Crossroads_33 pinned/moving.
static_assert(diagnosticPerformanceRoom>=-1&&diagnosticPerformanceRoom<=7,"Invalid performance room");
static constexpr int diagnosticPerformanceScene=diagnosticPerformanceRoom>=6?3:diagnosticPerformanceRoom%3;
static constexpr bool diagnosticPerformanceMoving=(diagnosticPerformanceRoom>=3&&diagnosticPerformanceRoom<=5)||diagnosticPerformanceRoom==7;
static_assert((gameFlagBackgroundCache&gameFlagHalfLight)==0,"Diagnostic flags must be independent");
static_assert(diagnosticGameFlags>=-1&&diagnosticGameFlags<=511,"Invalid diagnostic game flags");
// Set once from the standard benchmark tape before allocating graphics resources.
inline int replayGameFlags=-1;
inline int effectiveDiagnosticGameFlags() {
    return diagnosticsEnabled&&replayGameFlags>=0?replayGameFlags:diagnosticGameFlags;
}
inline bool sdFileExists(const char* path) {
    if(auto* file=std::fopen(path,"rb")){std::fclose(file);return true;}
    return false;
}
// fallback: the setting without an SD flag file (a default-on feature passes true).
inline bool gameFlagEnabled(const char* path,unsigned bit,bool fallback=false) {
    if(diagnosticsEnabled&&replayGameFlags>=0)return (unsigned(replayGameFlags)&bit)!=0;
    if constexpr(diagnosticGameFlags>=0)return (unsigned(diagnosticGameFlags)&bit)!=0;
    return sdFileExists(path)||fallback;
}
}
