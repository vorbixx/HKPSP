#include <3ds.h>
#include <cassert>
#include "gpu_queue.hpp"

namespace {
gxCmdEntry_s entries[hk3ds::gpuQueueCapacity];
gxCmdQueue_s* expanded=nullptr;
gxCmdQueue_s* boundQueue=nullptr;
gxCmdEntry_s* originalEntries=nullptr;
u16 originalCapacity=0;
gxCmdQueue_s prefix{};
bool prefixRunning=false;
}

extern "C" void __real_GX_BindQueue(gxCmdQueue_s* queue);

// Citro3D binds its empty queue during Init, and unbinds only after waiting,
// stopping and clearing it during Fini. Supply storage through libctru's
// public queue structure; restore ownership before Citro3D frees its context.
// GX command entries remain valid until frame completion, even after GPU
// execution: recycling a slot while a frame is being built is unsafe.
extern "C" void __wrap_GX_BindQueue(gxCmdQueue_s* queue)
{
    assert(!prefixRunning);
    if(queue!=expanded){
        if(expanded){
            assert(expanded->numEntries==0);
            expanded->entries=originalEntries;
            expanded->maxEntries=originalCapacity;
            expanded=nullptr;
        }
        if(queue&&queue->maxEntries<hk3ds::gpuQueueCapacity){
            assert(queue->numEntries==0);
            originalEntries=queue->entries;
            originalCapacity=queue->maxEntries;
            queue->entries=entries;
            queue->maxEntries=hk3ds::gpuQueueCapacity;
            expanded=queue;
        }
    }
    boundQueue=queue;
    __real_GX_BindQueue(queue);
}

bool hk3ds::drainGpuQueue()
{
    assert(!prefixRunning);
    if(!boundQueue)return false;
    gxCmdQueueRun(boundQueue);
    if(!gxCmdQueueWait(boundQueue,3000000000LL))return false;
    gxCmdQueueStop(boundQueue);
    return true;
}

// libctru runs one queue at a time and submits every entry up to numEntries.
// A snapshot of the bound queue (same storage, frozen count, no callback)
// therefore executes exactly the entries published so far, while GX commands
// issued afterwards only append to the idle bound queue.
static bool retireGpuQueuePrefix(s64 timeout)
{
    if(!prefixRunning)return true;
    if(!gxCmdQueueWait(&prefix,timeout))return false;
    gxCmdQueueStop(&prefix);
    assert(boundQueue&&boundQueue->entries==prefix.entries);
    assert(boundQueue->numEntries>=prefix.numEntries);
    // Copy the real cursors: a snapshot libctru declined to run executed nothing.
    boundQueue->curEntry=prefix.curEntry;
    boundQueue->lastEntry=prefix.lastEntry;
    prefixRunning=false;
    return true;
}

bool hk3ds::gpuQueuePrefixIdle()
{
    return retireGpuQueuePrefix(0);
}

bool hk3ds::startGpuQueuePrefix()
{
    if(!boundQueue||!retireGpuQueuePrefix(0))return false;
    if(boundQueue->curEntry!=boundQueue->lastEntry||
       boundQueue->lastEntry>=boundQueue->numEntries)return false;
    prefix=*boundQueue;
    // Citro3D's callback owns display swaps and frame-completion timing. It must
    // see only the final queue completion, never a temporarily empty prefix.
    prefix.callback=nullptr;prefix.user=nullptr;
    prefixRunning=true;
    gxCmdQueueRun(&prefix);
    return true;
}

void hk3ds::finishGpuQueuePrefix()
{
    // Unbounded, like Citro3D's own FrameBegin wait for the previous frame.
    retireGpuQueuePrefix(-1);
}
