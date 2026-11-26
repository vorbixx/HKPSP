#pragma once

namespace hk3ds {
// 26 room GrabPass sprites cost at most four GX submissions apiece, plus
// 32 reserved for camera passes, target clears and LCD transfers: 136 total.
// Keep headroom without allocating textures or stalling during a frame.
constexpr unsigned gpuQueueCapacity=256;
// Diagnostic only: finish queued entries without clearing/recycling their storage.
// Returns false after a bounded wait, leaving a timed-out queue running.
bool drainGpuQueue();
// Mid-frame start: run the entries queued so far without their completion
// callback; later entries stay queued for FrameEnd. Entry storage is shared, so
// the frame must call finishGpuQueuePrefix before Citro3D runs or clears the queue.
// Returns false, changing nothing, while an earlier prefix is still executing.
bool startGpuQueuePrefix();
// Non-blocking: true when no prefix is executing (a completed one is retired).
bool gpuQueuePrefixIdle();
// Wait for the prefix and advance the bound queue past the executed entries.
void finishGpuQueuePrefix();
}
