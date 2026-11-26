#include <3ds.h>
#include <citro2d.h>
#include "gpu_kick.hpp"
#include "gpu_queue.hpp"

// The linear-heap bounds Citro3D's FrameEnd(0) publishes before running a frame.
extern "C" {
extern u32 __ctru_linear_heap;
extern u32 __ctru_linear_heap_size;
}

bool hk3ds::kickGpuFrame()
{
    // An earlier prefix still occupies the GPU: starting sooner gains nothing,
    // and its entries must finish before the queue cursor can move.
    if(!gpuQueuePrefixIdle())return false;
    C2D_Flush();
    C3D_FrameSplit(0);
    // Same publication as FrameEnd(0): vertices, indices, command words and any
    // CPU-written texture become visible to the GPU before the prefix starts.
    // On failure the commands simply stay queued for the ordinary FrameEnd.
    if(R_FAILED(GSPGPU_FlushDataCache(reinterpret_cast<const void*>(__ctru_linear_heap),
                                      __ctru_linear_heap_size)))return false;
    return startGpuQueuePrefix();
}

void hk3ds::finishGpuKicks()
{
    finishGpuQueuePrefix();
}
