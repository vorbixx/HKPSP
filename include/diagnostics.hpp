#pragma once
#include "build_config.hpp"
#include <cstdint>

namespace hk3ds
{
// Independent progress word, not a coherent part of DiagnosticSnapshot.
enum class RuntimePhase : std::uint32_t {
    Unknown, CpuWait, Audio, Publish, GpuSync, GpuBegin, World, Hud,
    GpuEnd, DrawComplete, SceneLoad, Save, Cleanup, Input
};
// Read-only observer ABI for emulator RPC. No input or game-state writes.
// A reader accepts a sample only when both sequence numbers match and are even.
struct DiagnosticSnapshot
{
    std::uint32_t sequence, version, frame, status, resources, held, resets, overlay;
    std::uint32_t controlTicks, physicsTicks, overloads, attackSerial;
    std::int32_t health, soul, enemyHealth;
    std::uint32_t actor, dashing, doorBroken;
    float x, y, vx, vy, enemyX, enemyY;
    std::uint32_t roomPhase,deaths,retries,hitCount,killCount,brokenMask,exitGate,abilities;
    std::uint32_t visualLoaded,visualFailures,audioReady,audioPlayed,floorHits,greatDoorHits,collapsed,hazardRespawns;
    std::int32_t audioError;
    std::uint32_t linearFree;
    std::uint32_t replayActive,replayTick,replayTotal;
    std::uint32_t sequenceEnd;
};
static_assert(sizeof(DiagnosticSnapshot)==184, "Update observer ABI and reader together");
}
extern "C" { extern volatile hk3ds::DiagnosticSnapshot hk3ds_diagnostics; }
// Diagnostic shutdown handshake: HKEX requests normal cleanup; HKDN acknowledges
// capture-only completion after all session destructors. Does not inject controls.
extern "C" { extern volatile std::uint32_t hk3ds_test_shutdown; }
extern "C" { extern volatile std::uint32_t hk3ds_runtime_phase; }
namespace hk3ds {
inline void runtimePhase(RuntimePhase phase) {
#if HK3DS_DIAGNOSTICS
    hk3ds_runtime_phase=static_cast<std::uint32_t>(phase);
#else
    (void)phase;
#endif
}
}
