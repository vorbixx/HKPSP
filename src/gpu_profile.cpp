#include "gpu_profile.hpp"
#include "gpu_queue.hpp"
#include <citro2d.h>
#include <cmath>

// Same libctru heap base that Citro3D FrameEnd uses for its ordinary flush.
extern "C" u32* __ctru_linear_heap;

#if HK3DS_DIAGNOSTICS
namespace {
std::uint64_t ownerGpuTicks() {
    std::uint64_t ticks=0;
    const auto& row=*hk3ds::performanceObservation.row;
    for(unsigned i=unsigned(hk3ds::PerformanceSection::GpuBackground);
        i<unsigned(hk3ds::PerformanceSection::Count);++i)ticks+=row[i];
    return ticks;
}
}
hk3ds::GpuOwnerScope::GpuOwnerScope(const RoomOwnerDraw& draw) {
    auto& o=performanceObservation;
    if(!o.row||!o.serializedGpu||o.gpuStagesOnly||o.gpuProfileFailures)return;
    active_=o.owners.consider(draw,o.gpuFrameSelected);
    if(!active_)return;
    gpuProfileCheckpoint(PerformanceSection::GpuOwnerPrepare);
    before_=ownerGpuTicks();
}
hk3ds::GpuOwnerScope::~GpuOwnerScope() {
    if(!active_)return;
    gpuProfileCheckpoint(PerformanceSection::GpuOwner);
    auto& o=performanceObservation;
    if(o.gpuProfileFailures){o.owners.sample.valid=false;return;}
    o.owners.sample.ticks=std::uint32_t(std::min<std::uint64_t>(
        ownerGpuTicks()-before_,std::numeric_limits<std::uint32_t>::max()));
}
void hk3ds::gpuProfileCheckpoint(PerformanceSection section)
{
    auto& observation=performanceObservation;
    if(!observation.row||!observation.serializedGpu||!observation.gpuFrameSelected||
       observation.gpuProfileFailures)return;
    if(observation.gpuStagesOnly&&(section==PerformanceSection::GpuScene||
       section==PerformanceSection::GpuBackground||section==PerformanceSection::GpuDisplay))
        section=observation.gpuCategory;
    C2D_Flush();
    C3D_FrameSplit(0);
    // Splitting only queues commands. Publish C2D vertices and the command list
    // before running that queue, just as the ordinary FrameEnd does.
    if(R_FAILED(GSPGPU_FlushDataCache(__ctru_linear_heap,envGetLinearHeapSize()))){
        ++observation.gpuProfileFailures;return;
    }
    const auto start=observation.clock();
    if(!drainGpuQueue())++observation.gpuProfileFailures;
    auto& ticks=(*observation.row)[unsigned(section)];
    ticks=std::uint32_t(std::min<std::uint64_t>(std::numeric_limits<std::uint32_t>::max(),
        std::uint64_t(ticks)+observation.clock()-start));
}

void hk3ds::gpuProfileCategory(PerformanceSection section)
{
    auto& o=performanceObservation;
    if(!o.serializedGpu||!o.gpuStagesOnly||o.gpuCategory==section)return;
    gpuProfileCheckpoint(o.gpuCategory);
    o.gpuCategory=section;
}

void hk3ds::gpuProfileRoomCategory(std::uint32_t owner,bool behindHero)
{
    if(!performanceObservation.gpuStagesOnly)return;
    // Tutorial_01 build 22529139: fog/haze owners, excluding the three HeroLight
    // owners using the same sprites. Classification never affects draw culling.
    // Source room-visuals-map.json SHA256:
    // 9756e6330eeeb32f463bff9fac21f196460f10ca63939592410e1e95c9612613
    static constexpr std::uint32_t fog[]={84,133,244,279,426,545,676,861,941,971,
        982,985,1023,1160,1247,1413,1429,1496,1541,1874,1908,1913,2008,2071,
        2330,2332,2523,2531,2942,3114,3445,3514};
    gpuProfileCategory(std::binary_search(std::begin(fog),std::end(fog),owner)?
        PerformanceSection::GpuFogHaze:behindHero?PerformanceSection::GpuBackground:PerformanceSection::GpuScene);
}

std::uint64_t hk3ds::gpuProfileTransferBegin()
{
    const auto& o=performanceObservation;
    return o.row&&o.serializedGpu&&o.gpuStagesOnly&&o.gpuFrameSelected&&!o.gpuProfileFailures?o.clock():0;
}

void hk3ds::gpuProfileTransferEnd(PerformanceSection section,std::uint64_t start,bool drain)
{
    if(!start)return;
    auto& o=performanceObservation;
    if(drain&&!drainGpuQueue()){++o.gpuProfileFailures;return;}
    auto& ticks=(*o.row)[unsigned(section)];
    ticks=std::uint32_t(std::min<std::uint64_t>(std::numeric_limits<std::uint32_t>::max(),
        std::uint64_t(ticks)+o.clock()-start));
}

void hk3ds::gpuProfilePreviousFrame()
{
    auto& observation=performanceObservation;
    if(!observation.row||observation.serializedGpu)return;
    const double ticks=double(C3D_GetDrawingTime())*SYSCLOCK_ARM11/1000.0;
    if(!std::isfinite(ticks)||ticks<0){++observation.gpuProfileFailures;return;}
    (*observation.row)[unsigned(PerformanceSection::PreviousGpuFrame)]=
        std::uint32_t(std::min(ticks,double(std::numeric_limits<std::uint32_t>::max())));
}
#endif
