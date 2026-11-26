#pragma once
#include "sector_cache.hpp"
#include "sector_prefetch.hpp"
#include "room_renderer.hpp"
#include <3ds.h>
#include <atomic>

namespace hk3ds {
// The worker owns only its job. Cache, observer and GPU stay on the main thread.
class SectorStream {
    struct Job {
        std::unique_ptr<SectorData> data;
        std::atomic<bool> done{false};
        std::atomic<bool> cancel{false};
        bool ok=false,cancelled=false;
        unsigned pages=0,heapMax=0,linearMin=~0u;
        u64 readTicks=0,decodeTicks=0,metadataTicks=0;
        unsigned id=0;
        bool observed=false;
        std::atomic<unsigned> phase{0};
        u64 finishTick=0;
    };
    SectorCache cache_;
    std::unique_ptr<Job> job_;
    Thread worker_=nullptr;
    RoomVisualBank visuals_;
    int x_=-1,y_=-1,failedX_=-1,failedY_=-1;
    float previousX_=0,previousY_=0;
    bool initialized_=false;
    bool frameStarted_=false;
    bool verify_=false,configuredVerification_=false;
    unsigned hits_=0,misses_=0,prefetches_=0,failures_=0,verified_=0,cancelled_=0;
    unsigned nextJob_=0,observedJob_=0;
    unsigned observedReason_=0; // demand reservation, speculative reservation, insertion
    int observedX_=-1,observedY_=-1;
    static void run(void* context);
    void finish(bool wait);
    bool start(int x,int y,const SectorTargets* keep=nullptr);
public:
    ~SectorStream(){stop();}
    const RoomVisualBank* update(int x,int y,float cameraX,float cameraY,RoomRenderer& renderer);
    bool frameStarted() const{return frameStarted_;}
    unsigned failures() const{return failures_;}
    int x() const{return x_;} int y() const{return y_;}
    void stop();
};
}
