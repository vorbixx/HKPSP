#pragma once
#include "performance_sections.hpp"

namespace hk3ds {
#if HK3DS_DIAGNOSTICS
// Public GX queue drain; diagnostic wait includes dispatch/interrupt overhead.
// No-op unless the observer and PERF_DISABLE_MASK bit 0x40000000 are enabled.
void gpuProfileCheckpoint(PerformanceSection section);
// Sample only after FrameBegin has waited for the preceding frame to complete.
void gpuProfilePreviousFrame();
void gpuProfileCategory(PerformanceSection section);
void gpuProfileRoomCategory(std::uint32_t owner,bool behindHero);
std::uint64_t gpuProfileTransferBegin();
void gpuProfileTransferEnd(PerformanceSection section,std::uint64_t start,bool drain=false);
// One draw per sampled frame, from 16 ordinary + 16 destination-read rectangles.
// Includes serialized dispatch/interrupt overhead, and deliberately breaks batching.
class GpuOwnerScope {
    bool active_=false;
    std::uint64_t before_=0;
public:
    GpuOwnerScope(const RoomOwnerDraw& draw);
    ~GpuOwnerScope();
    GpuOwnerScope(const GpuOwnerScope&)=delete;
    GpuOwnerScope& operator=(const GpuOwnerScope&)=delete;
};
#else
inline void gpuProfileCheckpoint(PerformanceSection) {}
inline void gpuProfilePreviousFrame() {}
inline void gpuProfileCategory(PerformanceSection) {}
inline void gpuProfileRoomCategory(std::uint32_t,bool) {}
inline std::uint64_t gpuProfileTransferBegin() {return 0;}
inline void gpuProfileTransferEnd(PerformanceSection,std::uint64_t,bool=false) {}
struct GpuOwnerScope { explicit GpuOwnerScope(const RoomOwnerDraw&) {} };
#endif
}
