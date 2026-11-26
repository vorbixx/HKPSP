#pragma once
#ifndef HK3DS_GPU_KICK
#define HK3DS_GPU_KICK 7
#endif
namespace hk3ds {
// Build-time A/B selection (make GPU_KICK=<bits>); 0 restores one submission at FrameEnd.
enum class GpuKickPoint : unsigned { AfterBackground=1, BeforeActors=2, BeforeForeground=4, BeforePresent=8 };
inline constexpr unsigned gpuKickPoints=HK3DS_GPU_KICK;
static_assert(gpuKickPoints<=15,"GPU_KICK selects kick points 1|2|4|8");
constexpr bool gpuKickEnabled(GpuKickPoint point) {return (gpuKickPoints&unsigned(point))!=0;}
// Start the GPU on everything submitted so far while the CPU keeps emitting the
// rest of the frame. Commands and their order are unchanged; the call only ends
// the current command list early. Returns false (ordinary FrameEnd behavior)
// while a previous kick is still executing or when nothing could be published.
// Call only between C3D_FrameBegin and finishGpuKicks.
bool kickGpuFrame();
// Required once before C3D_FrameEnd: waits for the kicked work, so Citro3D runs
// only the remaining tail and its single display completion.
void finishGpuKicks();
}
