#include "render_memory.hpp"
#include <3ds.h>

namespace hk3ds {
std::size_t activeRoomTextureBudget=oldRoomTextureBudget;
}
extern "C" {
extern u32 __ctru_linear_heap_size;
extern u32 __ctru_heap_size;
void __real___system_allocateHeaps();
// APT/model queries are too late here. Select from the actual process allowance
// before libctru divides it into ordinary and linear heaps. The kernel layout
// also constrains emulators that grant Old models an oversized commit limit.
void __wrap___system_allocateHeaps() {
    Handle resource=0;
    s64 limit=0,committed=0;
    bool usageKnown=false;
    ResourceLimitType kind=RESLIMIT_COMMIT;
    if(R_SUCCEEDED(svcGetResourceLimit(&resource,CUR_PROCESS_HANDLE))) {
        const auto result=svcGetResourceLimitLimitValues(&limit,resource,&kind,1);
        usageKnown=R_SUCCEEDED(svcGetResourceLimitCurrentValues(&committed,resource,&kind,1));
        svcCloseHandle(resource);
        if(R_FAILED(result))limit=0;
    }
    const bool highDetail=limit>0&&hk3ds::highDetailRoomMemory(limit,osGetApplicationMemType());
    __ctru_linear_heap_size=highDetail?hk3ds::gameLinearHeapBytes:hk3ds::oldLinearHeapBytes;
    if(!highDetail) {
        const auto ordinary=limit>0&&usageKnown&&committed>=0?hk3ds::oldOrdinaryHeapBytes(limit,committed):0;
        if(!ordinary) { svcExitProcess(); } // Do not let zero restore automatic expanded heaps.
        __ctru_heap_size=ordinary;
    }
    hk3ds::activeRoomTextureBudget=highDetail?hk3ds::roomTextureBudget:hk3ds::oldRoomTextureBudget;
    __real___system_allocateHeaps();
}
}
