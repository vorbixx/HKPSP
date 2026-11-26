#pragma once
// Attribution exists only in the dedicated GPU binary and host oracle build.
#ifdef HKPSP_GPU_ATTRIB
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace hkpsp::gpu {
enum class Pass : unsigned { Far, Near, Actors, Lights, Grade, Hud, Count };
inline constexpr unsigned passCount=unsigned(Pass::Count),frames=150;
inline constexpr const char* passNames[]={"far","near","actors","lights","grade","hud"};
constexpr unsigned bit(Pass p){return 1u<<unsigned(p);}
inline constexpr unsigned all=(1u<<passCount)-1;
enum class Grade { Full, CopyOnly, OneSweep, Stale, Direct32, Vram, Target };
enum class Sweep { Triangles, Sprites, NoBlend0, Fill3, Clut8, Cached16 };
struct Config {const char* name;unsigned passes;bool nearest=false,vram=false,clutCache=false,passSync=false;Grade grade=Grade::Full;
    unsigned strip=0;Sweep sweep=Sweep::Triangles;};
inline constexpr std::array<Config,25> configs{{
    {"full",all}, {"grade_off",all&~bit(Pass::Grade)},
    {"lights_off",all&~bit(Pass::Lights)},
    {"grade_lights_off",all&~(bit(Pass::Grade)|bit(Pass::Lights))},
    {"nearest",all,true}, {"far_off",all&~bit(Pass::Far)},
    {"near_off",all&~bit(Pass::Near)}, {"hero_hud",bit(Pass::Actors)|bit(Pass::Hud)},
    {"vram",all,false,true}, {"clut_cached",all,false,false,true},
    {"pass_sync",all,false,false,false,true},
    {"grade_copy_only",all,false,false,false,true,Grade::CopyOnly},
    {"grade_one_sweep",all,false,false,false,true,Grade::OneSweep},
    {"grade_stale",all,false,false,false,true,Grade::Stale},
    {"grade_direct32",all,false,false,false,true,Grade::Direct32},
    {"grade_vram",all,false,false,false,true,Grade::Vram},
    {"grade_target",all,false,false,false,true,Grade::Target},
    {"target_strip64",all,false,false,false,true,Grade::Target,64},
    {"target_strip32",all,false,false,false,true,Grade::Target,32},
    {"target_strip16",all,false,false,false,true,Grade::Target,16},
    {"target_sprites",all,false,false,false,true,Grade::Target,0,Sweep::Sprites},
    {"target_noblend0",all,false,false,false,true,Grade::Target,0,Sweep::NoBlend0},
    {"fill_x3",all,false,false,false,true,Grade::Target,0,Sweep::Fill3},
    {"fill_clut8",all,false,false,false,true,Grade::Target,0,Sweep::Clut8},
    {"target_strip16_cached",all,false,false,false,true,Grade::Target,16,Sweep::Cached16}
}};
inline unsigned configIndex=0;
inline const Config& config(){return configs.at(configIndex);}
inline bool enabled(Pass p){return (config().passes&bit(p))!=0;}
using Pixels=std::array<std::uint64_t,passCount>;
}
#define HKPSP_GPU_PASS(list,group,...) do { \
    const auto gpuFirst=(list).quads.size(); HKPSP_PHASE(group,__VA_ARGS__); \
    (list).gpuPasses.resize(gpuFirst); \
    (list).gpuPasses.resize((list).quads.size(),hkpsp::gpu::Pass::group); \
} while(false)

#ifdef __PSP__
#include "perf.hpp"
namespace hkpsp::gpu {
struct Frame {
    perf::Row cpu{};
    unsigned ge=0,clutLoads=0,clutSkips=0,segments=0,vramBinds=0;
    std::array<unsigned,passCount> passUs{},passQuads{};
};
inline Frame current{};
inline std::array<std::array<Frame,frames>,configs.size()> records{};
inline std::array<bool,configs.size()> completed{};
inline std::array<unsigned,configs.size()> configUs{};
struct Residency {unsigned page,width,height,bits,bytes,firstFrame,offset;bool vram;};
inline std::vector<Residency> residency;
inline unsigned vramCapacity=0,vramUsed=0;
inline void finishConfig(){
    if(perf::recorded!=frames)throw std::runtime_error("Incomplete GPU config");
    for(unsigned f=0;f<frames;++f)records[configIndex][f].cpu=perf::rows[f];
    completed[configIndex]=true;
    configUs[configIndex]=perf::now()-perf::configStarted;
    perf::recorded=0;
}
inline void write(){
    for(const auto done:completed)if(!done)throw std::runtime_error("Incomplete GPU session");
    FILE* seconds=perf::open("config-seconds.csv");
    std::fprintf(seconds,"config,seconds\n");
    for(unsigned c=0;c<configs.size();++c)std::fprintf(seconds,"%s,%.6f\n",configs[c].name,configUs[c]/1000000.);
    perf::close(seconds);
    FILE* csv=perf::open("gpu.csv");
    std::fprintf(csv,"config,frame,ge_us,total_us,gu_us,sync_us,draw_list_us,clipping_us,quads,triangles,texture_binds,clut_loads,clut_skips,segments,vram_binds");
    for(const auto* p:passNames)std::fprintf(csv,",%s_us",p);
    for(const auto* p:passNames)std::fprintf(csv,",%s_quads",p);
    std::fprintf(csv,"\n");
    for(unsigned c=0;c<configs.size();++c)for(unsigned f=0;f<frames;++f){
        const auto& r=records[c][f];const auto& p=r.cpu;
        std::fprintf(csv,"%s,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u",configs[c].name,f,
            r.ge,p[perf::Total],p[perf::Gu],p[perf::Sync],p[perf::DrawList],p[perf::Clipping],
            p[perf::Quads],p[perf::Triangles],p[perf::TextureBinds],r.clutLoads,r.clutSkips,r.segments,r.vramBinds);
        for(const auto v:r.passUs)std::fprintf(csv,",%u",v);
        for(const auto v:r.passQuads)std::fprintf(csv,",%u",v);
        std::fprintf(csv,"\n");
    }
    perf::close(csv);
    FILE* summary=perf::open("summary.csv");FILE* notes=perf::open("summary.txt");
    std::fprintf(notes,"HKPSPGPU: %u configs x %u frames; CPU=333 MHz, bus=166 MHz.\n"
        "PPSSPP is functional/shape evidence only, never hardware performance.\n"
        "Each config resets simulation and replays the same tape from frame zero.\n"
        "All 150 frames included, including cold RAM upload/VRAM copy; no output I/O inside frames.\n"
        "One final-frame PPM per config is captured after perf::end, outside all recorded timings.\n"
        "ge_us measures sceGuFinish + sceGuSync wall time (residual GE wait, not isolated GPU execution).\n"
        "GU_DIRECT overlaps CPU command generation with GE execution. total_us includes draw preparation, waits, swap and simulation.\n"
        "pass_sync preserves draw order: sync/restart on every contiguous pass boundary, clear charged to far; waits accumulated by pass.\n"
        "Segmented waits remove overlap and change timing; they are not additive predictions for the full config.\n"
        "Six grade_* probes isolate each contiguous pass like pass_sync; grade_us is the isolated grade segment.\n"
        "grade_stale samples the preceding scene snapshot; next-snapshot upkeep is charged to the preceding scene segment; frame 0 bootstraps.\n"
        "grade_vram/grade_target reserve 557056 of 983040 free VRAM bytes, leaving 425984; no texture-page residency in these configs.\n"
        "grade_target renders the scene into the third buffer and grades into the back buffer, then overlays HUD.\n"
        "target_strip16_cached uses the shipped G3 path: one immutable 4320-byte mesh, three 16 px strip sweeps.\n"
        "clut_cached skips only consecutive identical CLUT pointer/count loads; page/palette changes always reload.\n"
        "vram.csv lists each first-visible textured page; first-fit, persistent, no eviction; palettes and grade snapshot stay in RAM.\n"
        "VRAM available=%u bytes, used=%u bytes; two 512x272 RGBA buffers reserved.\n"
        "p95 uses nearest rank; summary units are ms. Host geometric overdraw: gpu-overdraw*.csv.\n",
        unsigned(configs.size()),frames,vramCapacity,vramUsed);
    constexpr const char* heading="config,frames,mean_ge_ms,p95_ge_ms,mean_total_ms,p95_total_ms,mean_grade_ms,p95_grade_ms\n";
    std::fprintf(summary,"%s",heading);std::fprintf(notes,"%s",heading);
    for(unsigned c=0;c<configs.size();++c){
        std::array<unsigned,frames> ge{},total{},grade{};double gs=0,ts=0,cs=0;
        for(unsigned f=0;f<frames;++f){ge[f]=records[c][f].ge;total[f]=records[c][f].cpu[perf::Total];
            grade[f]=records[c][f].passUs[unsigned(Pass::Grade)];gs+=ge[f];ts+=total[f];cs+=grade[f];}
        std::sort(ge.begin(),ge.end());std::sort(total.begin(),total.end());std::sort(grade.begin(),grade.end());
        for(FILE* file:{summary,notes})std::fprintf(file,"%s,%u,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n",configs[c].name,frames,
            gs/frames/1000.,ge[(frames*95+99)/100-1]/1000.,ts/frames/1000.,total[(frames*95+99)/100-1]/1000.,
            cs/frames/1000.,grade[(frames*95+99)/100-1]/1000.);
    }
    perf::close(summary);perf::close(notes);
    FILE* pages=perf::open("vram.csv");
    std::fprintf(pages,"page,width,height,bits,bytes,first_visible_frame,location,offset\n");
    for(const auto& p:residency)std::fprintf(pages,"%u,%u,%u,%u,%u,%u,%s,%u\n",p.page,p.width,p.height,p.bits,p.bytes,
        p.firstFrame,p.vram?"VRAM":"RAM",p.offset);
    perf::close(pages);
}
}
#endif
#else
#define HKPSP_GPU_PASS(list,group,...) HKPSP_PHASE(group,__VA_ARGS__)
#endif
