#ifdef __PSP__
#include "../include/platform.hpp"
#include "animation.hpp"
#include "../include/inventory_draw.hpp"
#include "../include/test_mode.hpp"
#include "perf.hpp"
#include "page_residency.hpp"
#include "gpu_attrib.hpp"
#include "diag.hpp"
#include "../include/memory_report.hpp"
#include <pspkernel.h>
#include <pspdebug.h>
#include <psppower.h>
#include <pspiofilemgr.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <stdexcept>

#ifdef HKPSP_PERF
namespace hkpsp::perf {
unsigned runClock(int thread){
    const auto start=now();
    SceKernelThreadRunStatus status{};status.size=sizeof(status);
    const int result=thread<0?-1:sceKernelReferThreadRunStatus(thread,&status);
    current[ProbeCost]+=now()-start;++current[ProbeCalls];
    if(result<0){++current[RuntimeProbeErrors];return 0;}
    // Only the low word is needed: each difference spans at most one frame.
    return status.runClocks.low;
}
std::array<unsigned,4> runtime(){
    std::array<unsigned,4> result{{runClock(gameThread),runClock(mixerThread),runClock(ioThread),0}};
    const auto start=now();SceKernelSystemStatus status{};status.size=sizeof(status);
    if(sceKernelReferSystemStatus(&status)<0)++current[RuntimeProbeErrors];
    else result[3]=status.idleClocks.low;
    current[ProbeCost]+=now()-start;++current[ProbeCalls];return result;
}
#ifndef HKPSP_GPU_ATTRIB
void writeWorkSummary(FILE* file){
    std::fprintf(file,"Runtime counters: microseconds, low-word deltas; snapshots are sequential.\n"
        "kernel_interrupt_us - runtime_excess_us = total_us - game - mixer - audio I/O - idle.\n"
        "Residual includes untracked system threads and the diagnostic audio log thread; excess preserves negative residuals (snapshot skew).\n"
        "simulation_run_us excludes input; draw_list_run_us includes clipping; gu_run_us excludes clipping.\n"
        "cpu_scene/grade/hud_us: game run clocks between CPU submission markers, not GE completion markers.\n"
        "runtime_probe_us is the summed wall cost of runtime syscalls (subset of phase/bookkeeping times).\n"
        "perf_bookkeeping_us covers heap/counter collection and the end snapshot, not CSV output.\n"
        "no_streaming_reads=omitted;preload_pcm_bytes=%u;heap_cap_bytes=16777216\n"
        "Work percentiles exclude frame 0 per config; work_us = total_us - vblank_us.\n"
        "config,work_frames,work_median_us,work_p95_us,work_p99_us\n",preloadBytes);
    for(unsigned c=0;c<cpu::configs.size();++c){
        std::array<unsigned,frames-1> sorted{};
        for(unsigned f=1;f<frames;++f){const auto& r=cpu::records[c][f].row;sorted[f-1]=r[Total]-r[Vblank];}
        std::sort(sorted.begin(),sorted.end());
        std::fprintf(file,"%s,%u,%u,%u,%u\n",cpu::configs[c].name,frames-1,
            sorted[((frames-1)*50+99)/100-1],sorted[((frames-1)*95+99)/100-1],sorted[((frames-1)*99+99)/100-1]);
    }
}
#endif
}
#endif

#ifdef HKPSP_GPU_ATTRIB
PSP_MODULE_INFO("HKPSPGPU",0,1,0);
#elif defined(HKPSP_PERF)
PSP_MODULE_INFO("HKPSP perf",0,1,0);
#else
PSP_MODULE_INFO("HKPSP",0,1,0);
#endif
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER|THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_STACK_SIZE_KB(1024);
#ifdef HKPSP_HEAP_KB
PSP_HEAP_SIZE_KB(HKPSP_HEAP_KB);
#elif defined(HKPSP_DIAG) || defined(HKPSP_ROOM_GE_HEAP_TEST)
PSP_HEAP_SIZE_KB(16384);
#else
PSP_HEAP_SIZE_KB(-2048);
#endif
namespace hkpsp {std::unique_ptr<Platform> makePsp(unsigned frames,unsigned every,bool menus=false);}
#ifdef HKPSP_GPU_ATTRIB
namespace hkpsp {void captureGpu(Platform& platform);}
#endif
namespace {
#ifdef HKPSP_DIAG
constexpr const char* out="ms0:/PSP/GAME/HKPSPDIAG/out";
#elif defined(HKPSP_GPU_ATTRIB)
constexpr const char* out="ms0:/PSP/GAME/HKPSPGPU/out";
#elif defined(HKPSP_PERF)
constexpr const char* out="ms0:/PSP/GAME/HKPSPPERF/out";
#else
constexpr const char* out="ms0:/PSP/GAME/HKPSP/out";
#endif
int exitCallback(int,int,void*){sceKernelExitGame();return 0;}
int callbackThread(SceSize,void*){
    const int callback=sceKernelCreateCallback("HKPSP Exit",exitCallback,nullptr);
    if(callback>=0)sceKernelRegisterExitCallback(callback);
    sceKernelSleepThreadCB();return 0;
}
#ifndef HKPSP_PERF
unsigned count(const char* value,bool allowZero=false){
    char* end=nullptr;errno=0;const auto n=std::strtoul(value,&end,10);
    if(!*value||*end||errno||(!allowZero&&n<1)||n>1000000)throw std::runtime_error("Invalid test count");
    return static_cast<unsigned>(n);
}
#endif
}
int main(int argc,char** argv){
    const int clockResult=scePowerSetClockFrequency(333,333,166);
    sceIoMkdir(out,0777);
    // Run 3a (2026-11-16): every CPU phase was ~1.75x slower than run 2 while summary.txt printed a
    // constant 333 MHz. Record the clocks the PSP actually runs at (a CFW clock setting can override).
    if(const int fd=sceIoOpen((std::string(out)+"/clock.txt").c_str(),PSP_O_WRONLY|PSP_O_CREAT|PSP_O_TRUNC,0777);fd>=0){
        char text[128];
        const int n=std::snprintf(text,sizeof text,"set_result=%d\ncpu_mhz=%d\nbus_mhz=%d\n",clockResult,
            scePowerGetCpuClockFrequencyInt(),scePowerGetBusClockFrequencyInt());
        sceIoWrite(fd,text,unsigned(n));sceIoClose(fd);
    }
#ifdef HKPSP_PERF
    (void)argc;(void)argv;
    hkpsp::perf::gameThread=sceKernelGetThreadId();
    for(const auto* name:{"done.txt","error.txt","perf.csv","summary.txt"})
        sceIoRemove((std::string(out)+"/"+name).c_str());
#endif
#ifdef HKPSP_GPU_ATTRIB
    for(const auto* name:{"gpu.csv","summary.csv","vram.csv"})
        sceIoRemove((std::string(out)+"/"+name).c_str());
    for(const auto& c:hkpsp::gpu::configs)
        sceIoRemove((std::string(out)+"/"+c.name+".ppm").c_str());
#endif
    try{
        const int thread=sceKernelCreateThread("HKPSP callbacks",callbackThread,0x11,0x1000,0,nullptr);
        if(thread<0||sceKernelStartThread(thread,0,nullptr)<0)throw std::runtime_error("Cannot start exit callback");
#ifdef HKPSP_PERF
        const unsigned frames=hkpsp::perf::frames,every=1;const bool test=true;
#else
        unsigned frames=300,every=30;bool test=false;
        const char* config="ms0:/PSP/GAME/HKPSP/data/test-mode.cfg";
        SceIoStat stat{};
        if(sceIoGetstat(config,&stat)>=0){
            test=true;
            auto* f=std::fopen(config,"rb");
            if(!f)throw std::runtime_error("Cannot open test-mode.cfg");
            char frameText[32]{},intervalText[32]{},preview[32]{},extra[2]{};
            const int fields=std::fscanf(f,"frames=%31s capture_interval=%31s %31s %1s",frameText,intervalText,preview,extra);
            std::fclose(f);
            if(fields==3&&std::strcmp(preview,"inventory=items")==0)
                hkpsp::inventoryPreview=hkpsp::InventoryPreview::Items;
            else if(fields==3&&std::strcmp(preview,"inventory=charms")==0)
                hkpsp::inventoryPreview=hkpsp::InventoryPreview::Charms;
            else if(fields==3&&std::strcmp(preview,"restore=save")==0)
                hkpsp::captureRestoresSave=true;
            else if(fields!=2)throw std::runtime_error("Invalid test-mode.cfg");
            frames=count(frameText);every=count(intervalText,true);
        }
        for(int i=1;i<argc;++i){
            const std::string arg=argv[i];test=true;
            if(arg=="--test")continue;
            if((arg!="--frames"&&arg!="--capture-interval")||i+1>=argc)
                throw std::runtime_error("Usage: --test [--frames N] [--capture-interval N]");
            const auto n=count(argv[++i],arg=="--capture-interval");
            if(arg=="--frames")frames=n;else every=n;
        }
#endif
        HKPSP_DIAG_ONLY(hkpsp::diag::initialize(frames,every,test);)
        // Test runs only: play mode must leave out/ empty (test_backend_ppsspp play check).
        hkpsp::memory_report::install(out,test||hkpsp::memory_report::forcedFailureRequested());
        if(test||hkpsp::memory_report::forcedFailureRequested()){
            hkpsp::memory_report::writeBootReport(out);
            if(hkpsp::memory_report::forcedFailureRequested())
                throw hkpsp::memory_report::AllocationFailure("test-forced",1048576);
        }
        hkpsp::GameRunResult result;
#ifdef HKPSP_GPU_ATTRIB
        // Attribute GPU variants on the same CPU path as the shipped game.
        hkpsp::cpu::configIndex=hkpsp::cpu::shippedIndex;
        for(unsigned c=0;c<hkpsp::gpu::configs.size();++c){
            hkpsp::gpu::configIndex=c;
            {auto platform=hkpsp::makePsp(frames,every);
                auto reported=hkpsp::memory_report::instrument(*platform,out,true);
                const auto run=hkpsp::runGame(*reported,frames);
                result.frames+=run.frames;result.controlSteps+=run.controlSteps;
                hkpsp::captureGpu(*platform);}
            hkpsp::gpu::finishConfig();
        }
        hkpsp::gpu::write();
#else
        // capture_interval=0: bounded ordinary-input replay on the shipped
        // backend, with M1 reports but no readback buffer or audio log queues.
        {auto platform=hkpsp::makePsp(test&&every?frames:0,every,!test);
            auto reported=hkpsp::memory_report::instrument(*platform,out,test);
            result=hkpsp::runGame(*reported,test?frames:0);}
        HKPSP_PROFILE(hkpsp::perf::write();)
#endif
        HKPSP_DIAG_ONLY(hkpsp::diag::shutdown();)
        if(test){
            auto* done=std::fopen((std::string(out)+"/done.txt").c_str(),"wb");
            if(!done)throw std::runtime_error("Cannot write done marker");
#ifdef HKPSP_GPU_ATTRIB
            std::fprintf(done,"configs=%u\nframes_per_config=%u\nframes=%u\ncontrol_steps=%u\n",
                unsigned(hkpsp::gpu::configs.size()),frames,result.frames,result.controlSteps);
#else
            std::fprintf(done,"frames=%u\ncontrol_steps=%u\n",result.frames,result.controlSteps);
#endif
            std::fprintf(done,"animation_missing_plays=%u\n",unsigned(hk3ds::animationMissingPlayCount()));
            hkpsp::writePageDone(done);
            if(std::fclose(done))throw std::runtime_error("Cannot close done marker");
        }
    }catch(const std::exception& error){
        // IoFailure already owns its pre-unwind snapshot. Do not allocate FILE
        // storage or a second string when libc may be the failing resource.
        std::string formatted;
        const auto* io=dynamic_cast<const hkpsp::memory_report::IoFailure*>(&error);
        if(!io)formatted=hkpsp::memory_report::failureText(error);
        const char* text=io?io->what():formatted.c_str();
        char path[320];std::snprintf(path,sizeof path,"%s/error.txt",out);
        const int fd=sceIoOpen(path,PSP_O_WRONLY|PSP_O_CREAT|PSP_O_TRUNC,0777);
        if(fd>=0){
            const char* next=text;unsigned left=std::strlen(text);
            while(left){const int n=sceIoWrite(fd,next,left);if(n<=0)break;next+=n;left-=unsigned(n);}
            sceIoClose(fd);
        }
        pspDebugScreenInit();pspDebugScreenPrintf("%s",text);
        sceKernelDelayThread(10000000);
    }
    sceKernelExitGame();return 0;
}
#endif
