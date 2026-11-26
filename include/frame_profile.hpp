#pragma once
#include "build_config.hpp"
#if !HK3DS_DIAGNOSTICS
#include "release_observers.hpp"
#include <array>
namespace hk3ds { inline thread_local std::array<u64,3>* simulationProfileDetails=nullptr; }
#else
#include "audio_evidence.hpp"
#include "graphics_profile.hpp"
// Optional bounded observer; file output happens only after the game loop.
#include "platform.hpp"
#include "performance_sections.hpp"
#include <3ds.h>
#include <malloc.h>
#include <array>
#include <algorithm>
#include <cstdio>
#include <memory>
#include <new>

namespace hk3ds {
inline thread_local std::array<u64,3>* simulationProfileDetails=nullptr;
class FrameProfile {
public:
    void roomVram(unsigned pages,unsigned bytes,unsigned effectsBytes) {
        roomVramPages_=pages;roomVramBytes_=bytes;effectsVramBytes_=effectsBytes;
        vramFree_=vramSpaceFree();
    }
    enum Stage { Input, Logic, Save, Audio, SectorWait, Metadata, Page, Draw,
                 FrameBegin, FrameEnd, Diagnostics, Observer, LoadWait, GpuWait, Count };
    struct Row {
        PerformanceObservation::Ticks sections{};
        PerformanceObservation::Coverage coverage{};
        RoomOwnerSample owner{};
        unsigned disabledFeatures=0,replayTick=0;
        u64 copyTicks=0,flushTicks=0,eventObserverTicks=0;
        unsigned uploadPages=0,uploadBytes=0;
        std::array<unsigned,5> deferred{}; // busy, invalid, resident, failed target, reserve
        std::array<u64,Count> ticks{};
        std::array<u64,3> detail{};
        u64 total=0, gap=0, startTick=0;
        unsigned frame=0, phase=0, overloads=0, linearMin=~0u, heapMax=0;
        unsigned pages=0, bytes=0;
        int oldX=-1,oldY=-1,x=-1,y=-1;
        u64 readTicks=0,decodeTicks=0,metadataTicks=0,saveTicks=0;
        unsigned cacheBudget=0,cacheUsed=0,hits=0,misses=0,prefetches=0,failures=0;
        u64 obsoleteWaitTicks=0;
        u64 simulationTicks=0,snapshotTicks=0,simulationWaitTicks=0;
        unsigned cancelled=0;
    };
    struct Scope {
        FrameProfile& owner; Stage stage; u64 start;
        Scope(FrameProfile& p,Stage s):owner(p),stage(s),start(p.tick()){}
        ~Scope(){if(start)owner.current_.ticks[stage]+=svcGetSystemTick()-start;}
    };
    struct Detail {
        FrameProfile& owner; unsigned index; u64 start;
        Detail(FrameProfile& p,unsigned i):owner(p),index(i),start(p.tick()){}
        ~Detail(){if(start)(simulationProfileDetails?*simulationProfileDetails:owner.current_.detail)[index]+=svcGetSystemTick()-start;}
    };
    struct UploadScope {
        FrameProfile& owner; bool flush; u64 start;
        UploadScope(FrameProfile& p,bool f):owner(p),flush(f),start(p.tick()){}
        ~UploadScope(){if(start)(flush?owner.current_.flushTicks:owner.current_.copyTicks)+=svcGetSystemTick()-start;}
    };
private:
    unsigned roomVramPages_=0,roomVramBytes_=0,effectsVramBytes_=0,vramFree_=0;
    struct Event {u64 tick; unsigned job,kind,phase; int x,y; unsigned value;};
    static constexpr unsigned eventCapacity=32768;
    std::unique_ptr<Event[]> events_;
    unsigned eventCount_=0,eventDropped_=0;
    u64 epoch_=0;
    // Keep the original observer heap budget as nested observations grow.
    static constexpr unsigned capacity=16384*(sizeof(Row)-sizeof(PerformanceObservation::Ticks)-
        sizeof(PerformanceObservation::Coverage)-sizeof(unsigned))/sizeof(Row);
    unsigned rowCapacity_=capacity,eventCapacity_=eventCapacity;
    std::unique_ptr<Row[]> rows_;
    Row current_{};
    unsigned count_=0,dropped_=0;
    u64 start_=0,end_=0;
public:
    bool enabled() const {return bool(rows_);}
    bool performanceFeaturesOverride=false;
    int diagnosticGameFlags=-1,diagnosticPerformanceRoom=-1;
    unsigned fogTrimDraws=0,fogTrimQuads=0,backgroundWidth=0;
    void simulation(u64 logic,u64 snapshot,u64 wait,const PerformanceObservation::Ticks& sections,const std::array<u64,3>& details,bool asynchronous){
        if(!rows_)return;
        current_.simulationTicks=logic;current_.snapshotTicks=snapshot;current_.simulationWaitTicks=wait;
        if(!asynchronous)current_.ticks[Logic]+=logic+snapshot;
        // Worker work overlaps draw; these times are not independent main-thread stages.
        for(unsigned i=0;i<performanceSectionCount;++i)current_.sections[i]+=sections[i];
        for(unsigned i=0;i<details.size();++i)current_.detail[i]+=details[i];
    }
    u64 tick() const {return rows_?svcGetSystemTick():0;}
    void begin(bool force=false) {
        auto* f=std::fopen("sdmc:/3ds/HK3DS/console-metrics.flag","rb");
        if(!f&&!force)return;
        if(f)std::fclose(f);
        const unsigned divisor=graphicsProfile().diagnosticTraceDivisor;
        rowCapacity_=capacity/divisor;eventCapacity_=eventCapacity/divisor;
        rows_.reset(new(std::nothrow) Row[rowCapacity_]);
        if(rows_)events_.reset(new(std::nothrow) Event[eventCapacity_]);
        if(!events_)rows_.reset();
        performanceObservation.clock=[]()->std::uint64_t{return svcGetSystemTick();};
        performanceObservation.row=rows_?&current_.sections:nullptr;
        performanceObservation.coverage=rows_?&current_.coverage:nullptr;
        epoch_=start_=tick();
    }
    void memory() {
        if(!rows_)return;
        current_.linearMin=std::min(current_.linearMin,unsigned(linearSpaceFree()));
        current_.heapMax=std::max(current_.heapMax,unsigned(mallinfo().uordblks));
    }
    void page(unsigned bytes) {++current_.pages;current_.bytes+=bytes;memory();}
    void uploaded(unsigned pages,unsigned bytes){if(rows_){current_.uploadPages+=pages;current_.uploadBytes+=bytes;}}
    void deferred(unsigned reason){if(rows_)++current_.deferred[reason];}
    // kind: 1 start, 2 demand, 3 worker finish, 4 budget reject, 5 protected reject,
    // 6 eviction, 7 thread fallback, 8 reservation attempt.
    // Worker phase: 0 queued, 1 metadata, 2 read, 3 decode, 4 done.
    // Reservation/eviction phase: 0 demand scratch, 1 speculative scratch, 2 insertion.
    void event(unsigned kind,unsigned job,int x,int y,unsigned phase=0,unsigned value=0,u64 at=0){
        if(!rows_)return;
        const auto begin=svcGetSystemTick();
        if(eventCount_<eventCapacity_)events_[eventCount_++]={at?at:begin,job,kind,phase,x,y,value};
        else ++eventDropped_;
        current_.eventObserverTicks+=svcGetSystemTick()-begin;
    }
    void worker(u64 read,u64 decode,u64 metadata,unsigned pages,unsigned heap,unsigned linear){
        current_.readTicks+=read;current_.decodeTicks+=decode;current_.pages+=pages;
        current_.metadataTicks+=metadata;
        current_.bytes+=pages*2*1024*1024;
        current_.heapMax=std::max(current_.heapMax,heap);current_.linearMin=std::min(current_.linearMin,linear);
    }
    void saved(u64 ticks){current_.saveTicks+=ticks;}
    // Nested in LoadWait; do not add it to the independent stage sum.
    void obsoleteWait(u64 start){if(start)current_.obsoleteWaitTicks+=svcGetSystemTick()-start;}
    void cache(unsigned budget,unsigned used,unsigned hits,unsigned misses,unsigned prefetches,unsigned failures,unsigned cancelled=0){
        current_.cacheBudget=budget;current_.cacheUsed=used;current_.hits=hits;
        current_.misses=misses;current_.prefetches=prefetches;current_.failures=failures;
        current_.cancelled=cancelled;
    }
    void commit(unsigned frame,unsigned phase,unsigned overloads,int x,int y,unsigned replayTick=0) {
        if(!rows_)return;
        current_.owner=performanceObservation.serializedGpu&&performanceObservation.gpuFrameSelected?
            performanceObservation.owners.sample:RoomOwnerSample{};
        const auto observer=svcGetSystemTick();memory();
        current_.startTick=start_-epoch_;current_.frame=frame;current_.phase=phase;current_.overloads=overloads;
        current_.disabledFeatures=performanceObservation.disabled;current_.replayTick=replayTick;
        current_.x=x;current_.y=y;
        current_.ticks[Observer]+=svcGetSystemTick()-observer;
        const auto now=svcGetSystemTick();current_.total=now-start_;
        if(count_<rowCapacity_)rows_[count_++]=current_;else ++dropped_;
        end_=svcGetSystemTick();
    }
    void next(int x,int y) {
        if(!rows_)return;
        current_=Row{};start_=svcGetSystemTick();
        current_.gap=start_-end_;current_.oldX=x;current_.oldY=y;
    }
    bool finish(const PlatformPerformance& platform,const char* output=nullptr,
                void(*stopAudio)(void*)=nullptr,void* audioContext=nullptr) {
        if(!rows_)return false;
        char path[128];std::snprintf(path,sizeof(path),"sdmc:/3ds/HK3DS/stages-%llu.csv",(unsigned long long)osGetTime());
        auto* f=std::fopen(output?output:path,"wx");if(!f)return false;
        bool ok=std::fprintf(f,"# hk3ds-stages-v3 observer_bytes=%u dropped=%u heap_size=%u linear_size=%u event_dropped=%u row_bytes=%u event_bytes=%u tick_hz=%u",unsigned(sizeof(Row)*rowCapacity_+sizeof(Event)*eventCapacity_),dropped_,unsigned(envGetHeapSize()),unsigned(envGetLinearHeapSize()),eventDropped_,unsigned(sizeof(Row)),unsigned(sizeof(Event)),unsigned(SYSCLOCK_ARM11))>0;
        ok&=std::fprintf(f," submitted_quads=%u identity_quads=%u flat_quads=%u",
            performanceObservation.submittedQuads,performanceObservation.identityQuads,performanceObservation.flatQuads)>0;
        ok&=std::fprintf(f," diagnostic_game_flags=%d performance_features_override=%u diagnostic_perf_room=%d",
            diagnosticGameFlags,unsigned(performanceFeaturesOverride),diagnosticPerformanceRoom)>0;
        ok&=std::fprintf(f," fog_trim_draws=%u fog_trim_quads=%u background_width=%u",
            fogTrimDraws,fogTrimQuads,backgroundWidth)>0;
        ok&=std::fprintf(f," gpu_serialized=%u gpu_profile_failures=%u gpu_sample_stride=%u gpu_stages_only=%u",
            unsigned(performanceObservation.serializedGpu),performanceObservation.gpuProfileFailures,
            performanceObservation.gpuStagesOnly?1u:gpuProfileStride,unsigned(performanceObservation.gpuStagesOnly))>0;
        ok&=std::fprintf(f," cpu_profile=%u cpu_sample_stride=%u",
            unsigned(performanceObservation.cpuProfileEnabled),gpuProfileStride)>0;
        ok&=std::fprintf(f," room_vram_pages=%u room_vram_bytes=%u effects_vram_bytes=%u vram_free=%u",
            roomVramPages_,roomVramBytes_,effectsVramBytes_,vramFree_)>0;
        ok&=writePlatformPerformance(f,platform);
        ok&=std::fprintf(f,"frame,phase,old_x,old_y,x,y,total_ms,gap_ms,input_ms,logic_ms,save_ms,audio_ms,sector_wait_ms,metadata_ms,page_ms,draw_ms,begin_ms,end_ms,diagnostics_ms,observer_ms,load_wait_ms,gpu_wait_ms,remainder_ms,linear_min,heap_max,pages,bytes,overloads,contacts_ms,ccd_ms,queries_ms,worker_read_ms,worker_decode_ms,cache_budget,cache_used,cache_hits,cache_misses,prefetches,cache_failures,worker_metadata_ms,worker_save_ms,obsolete_wait_ms,cancelled_jobs,upload_copy_ms,upload_flush_ms,upload_pages,upload_bytes,defer_busy,defer_invalid,defer_resident,defer_failed,defer_reserve,start_tick,event_observer_ms,disabled_features")>0;
        // Append nested columns without changing the v3 independent stage order.
        for(const auto* name:performanceSectionNames)ok&=std::fprintf(f,",%s",name)>0;
        ok&=std::fprintf(f,",room_quads,room_background_pixels,room_foreground_pixels")>0;
        ok&=std::fprintf(f,",simulation_ms,snapshot_ms,simulation_wait_ms")>0;
        ok&=std::fprintf(f,",owner_valid,owner_id,owner_draw_index,owner_material,owner_pass,owner_pixels,owner_gpu_ms,background_refreshes,background_reuses,background_camera_x,background_camera_y,replay_tick,gpu_kicks\n")>0;
        const auto ms=[](u64 t){return 1000.0*double(t)/SYSCLOCK_ARM11;};
        for(unsigned i=0;i<count_;++i){const auto& r=rows_[i];u64 sum=0;
            ok&=std::fprintf(f,"%u,%u,%d,%d,%d,%d,%.6f,%.6f",r.frame,r.phase,r.oldX,r.oldY,r.x,r.y,ms(r.total),ms(r.gap))>0;
            for(auto t:r.ticks){sum+=t;ok&=std::fprintf(f,",%.6f",ms(t))>0;}
            ok&=std::fprintf(f,",%.6f,%u,%u,%u,%u,%u",ms(r.total>=sum?r.total-sum:0),r.linearMin,r.heapMax,r.pages,r.bytes,r.overloads)>0;
            for(auto t:r.detail)ok&=std::fprintf(f,",%.6f",ms(t))>0;
            ok&=std::fprintf(f,",%.6f,%.6f,%u,%u,%u,%u,%u,%u",ms(r.readTicks),ms(r.decodeTicks),r.cacheBudget,r.cacheUsed,r.hits,r.misses,r.prefetches,r.failures)>0;
            ok&=std::fprintf(f,",%.6f,%.6f,%.6f,%u",ms(r.metadataTicks),ms(r.saveTicks),ms(r.obsoleteWaitTicks),r.cancelled)>0;
            ok&=std::fprintf(f,",%.6f,%.6f,%u,%u",ms(r.copyTicks),ms(r.flushTicks),r.uploadPages,r.uploadBytes)>0;
            for(auto n:r.deferred)ok&=std::fprintf(f,",%u",n)>0;
            ok&=std::fprintf(f,",%llu,%.6f",(unsigned long long)r.startTick,ms(r.eventObserverTicks))>0;
            ok&=std::fprintf(f,",%u",r.disabledFeatures)>0;
            for(auto ticks:r.sections)ok&=std::fprintf(f,",%.6f",ms(ticks))>0;
            ok&=std::fprintf(f,",%u,%u,%u",unsigned(r.coverage.quads),
                unsigned(r.coverage.backgroundPixels),unsigned(r.coverage.foregroundPixels))>0;
            ok&=std::fprintf(f,",%.6f,%.6f,%.6f",ms(r.simulationTicks),ms(r.snapshotTicks),ms(r.simulationWaitTicks))>0;
            ok&=std::fprintf(f,",%u,%u,%u,%u,%u,%u,%.6f",unsigned(r.owner.valid),
                unsigned(r.owner.draw.owner),unsigned(r.owner.draw.index),
                unsigned(r.owner.draw.material),unsigned(r.owner.draw.pass),
                unsigned(r.owner.draw.pixels),ms(r.owner.ticks))>0;
            ok&=std::fprintf(f,",%u,%u,%.6f,%.6f",unsigned(r.coverage.backgroundRefreshes),
                unsigned(r.coverage.backgroundReuses),r.coverage.backgroundCameraX,r.coverage.backgroundCameraY)>0;
            ok&=std::fprintf(f,",%u,%u\n",r.replayTick,unsigned(r.coverage.gpuKicks))>0;
        }
        for(unsigned i=0;i<eventCount_;++i){const auto& e=events_[i];
            ok&=std::fprintf(f,"# event,%llu,%u,%u,%u,%d,%d,%u\n",(unsigned long long)(e.tick-epoch_),e.job,e.kind,e.phase,e.x,e.y,e.value)>0;
        }
        ok&=std::fprintf(f,"# events complete count=%u\n",eventCount_)>0;
        ok&=audioEvidence.write(f);
        // Keep ambience audible during the evidence flush, then record its
        // actual application stop clock in the same independently retained log.
        if(stopAudio){stopAudio(audioContext);ok&=audioEvidence.writeOutputStop(f);}
        ok&=std::fprintf(f,"# complete rows=%u\n",count_)>0;
        if(std::fclose(f))ok=false;
        std::printf("Stage metrics: %s\n",ok?"saved":"WRITE FAILED");
        return ok&&dropped_==0&&eventDropped_==0&&count_>0;
    }
};
inline FrameProfile frameProfile;
}

#endif
