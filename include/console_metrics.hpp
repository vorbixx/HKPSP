#pragma once
#include "build_config.hpp"
#if !HK3DS_DIAGNOSTICS
#include "release_observers.hpp"
#else
// Optional hardware observer. No SD writes in the measured loop.
#include "diagnostics.hpp"
#include "platform.hpp"
#include <3ds.h>
#include <citro2d.h>
#include <malloc.h>
#include <cstdio>
#include <memory>
#include <new>
#include <algorithm>

namespace hk3ds
{
class ConsoleMetrics
{
    struct Row
    {
        unsigned frames=0, first=0, last=0, phase=0, deaths=0, retries=0, exit=0, replay=0;
        unsigned linearMin=~0u, heapUsedMax=0, heapFreeMin=~0u, overloads=0;
        unsigned audioReady=1, audioPlayed=0, visualFailures=0, held=0;
        int audioError=0, sectorX=-1, sectorY=-1, health=0;
        double sumMs=0, maxMs=0;
        float cpuMax=0, gpuMax=0, x=0, y=0;
    };
    static constexpr unsigned capacity=1024;
    std::unique_ptr<Row[]> rows_;
    Row current_{};
    unsigned count_=0, dropped_=0;
    float commandBufferMax_=0;
    u64 started_=0, previous_=0;
    void commit()
    {
        if(!current_.frames)return;
        if(count_<capacity)rows_[count_++]=current_;else ++dropped_;
        current_=Row{};
    }
public:
    void begin(bool force=false)
    {
        auto* flag=std::fopen("sdmc:/3ds/HK3DS/console-metrics.flag","rb");
        if(!flag&&!force)return;
        if(flag)std::fclose(flag);
        rows_.reset(new(std::nothrow) Row[capacity]);
        if(!rows_){std::printf("Console metrics: allocation failed\n");return;}
        started_=previous_=svcGetSystemTick();
    }
    void sample(const volatile DiagnosticSnapshot& d,int sx,int sy)
    {
        if(!rows_)return;
        const auto now=svcGetSystemTick();
        const double ms=1000.0*double(now-previous_)/SYSCLOCK_ARM11;previous_=now;
        if(current_.frames&&(current_.phase!=d.roomPhase||current_.sectorX!=sx||current_.sectorY!=sy))commit();
        auto& r=current_;
        if(!r.frames)r.first=d.frame;
        ++r.frames;r.last=d.frame;r.sumMs+=ms;r.maxMs=std::max(r.maxMs,ms);
        r.cpuMax=std::max(r.cpuMax,C3D_GetProcessingTime());
        r.gpuMax=std::max(r.gpuMax,C3D_GetDrawingTime());
        commandBufferMax_=std::max(commandBufferMax_,C3D_GetCmdBufUsage());
        const auto heap=mallinfo();
        r.heapUsedMax=std::max(r.heapUsedMax,unsigned(heap.uordblks));
        r.heapFreeMin=std::min(r.heapFreeMin,unsigned(heap.fordblks));
        r.linearMin=std::min(r.linearMin,unsigned(d.linearFree));
        r.phase=d.roomPhase;r.deaths=d.deaths;r.retries=d.retries;r.exit|=d.exitGate;r.replay|=d.replayActive;
        r.overloads=std::max(r.overloads,unsigned(d.overloads));r.audioReady&=d.audioReady;
        if(d.audioError)r.audioError=d.audioError;
        r.audioPlayed=std::max(r.audioPlayed,unsigned(d.audioPlayed));
        r.visualFailures=std::max(r.visualFailures,unsigned(d.visualFailures));r.held|=d.held;
        r.health=d.health;r.x=d.x;r.y=d.y;r.sectorX=sx;r.sectorY=sy;
        if(r.frames>=60)commit();
    }
    bool finish(const PlatformPerformance& platform,const char* output=nullptr)
    {
        if(!rows_)return false;
        commit();
        char path[128];std::snprintf(path,sizeof(path),"sdmc:/3ds/HK3DS/console-%llu.csv",static_cast<unsigned long long>(osGetTime()));
        auto* f=std::fopen(output?output:path,"wx");
        if(!f){std::printf("Console metrics: cannot create CSV\n");return false;}
        bool ok=std::fprintf(f,"# hk3ds-console-v1 observer_bytes=%u dropped=%u elapsed_ms=%.3f command_buffer_usage_max=%.6f",unsigned(sizeof(Row)*capacity),dropped_,1000.0*double(svcGetSystemTick()-started_)/SYSCLOCK_ARM11,commandBufferMax_)>0;
        ok&=writePlatformPerformance(f,platform);
        ok&=std::fprintf(f,"first,last,frames,phase,sector_x,sector_y,mean_ms,max_ms,c3d_cpu_max_ms,c3d_gpu_max_ms,linear_min,heap_used_max,heap_free_min,overloads,deaths,retries,exit_gate,replay,audio_ready,audio_error,audio_played,visual_failures,held,health,x,y\n")>0;
        for(unsigned i=0;i<count_;++i)
        {
            const auto& r=rows_[i];
            ok&=std::fprintf(f,"%u,%u,%u,%u,%d,%d,%.6f,%.6f,%.6f,%.6f,%u,%u,%u,%u,%u,%u,%u,%u,%u,%d,%u,%u,%u,%d,%.6f,%.6f\n",r.first,r.last,r.frames,r.phase,r.sectorX,r.sectorY,r.sumMs/r.frames,r.maxMs,r.cpuMax,r.gpuMax,r.linearMin,r.heapUsedMax,r.heapFreeMin,r.overloads,r.deaths,r.retries,r.exit,r.replay,r.audioReady,r.audioError,r.audioPlayed,r.visualFailures,r.held,r.health,r.x,r.y)>0;
        }
        ok&=std::fprintf(f,"# complete rows=%u\n",count_)>0;
        if(std::fclose(f)!=0)ok=false;
        std::printf("Console metrics: %s (%s)\n",ok?"saved":"WRITE FAILED",path);
        return ok&&dropped_==0&&count_>0;
    }
};
}

#endif
