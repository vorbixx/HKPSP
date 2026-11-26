#pragma once
#define HKPSP_RENDER_PROBE
#include <array>
namespace hkpsp::cpu {
// Room candidate ordering/dense-owner A/B, independent of GU emission.
// Configure before constructing RoomIndex; keep it fixed for that room.
#ifdef HKPSP_ROOM_CANDIDATES_LEGACY
inline bool roomCandidates=false;
#else
inline bool roomCandidates=true;
#endif
// Exact room/quad emission A/B. Rebuild with HKPSP_ROOM_EMISSION_LEGACY
// to retain the original GU commands, batching and intermediate vertex vector.
#ifdef HKPSP_ROOM_EMISSION_LEGACY
inline bool roomEmission=false;
#else
inline bool roomEmission=true;
#endif
// Compile the hardware A/B off build with -DHKPSP_CLUT_REORDER_OFF.
// Mutable on host so the focused test can replay identical lists both ways.
#ifdef HKPSP_CLUT_REORDER_OFF
inline bool clutReorder=false;
#else
inline bool clutReorder=true;
#endif
// Exact HUD A/B: disables both span reuse and GE mask batching. Rebuild all
// objects with HKPSP_HUD_LIQUID_LEGACY for the original hardware cost path.
#ifdef HKPSP_HUD_LIQUID_LEGACY
inline bool hudLiquid=false;
#else
inline bool hudLiquid=true;
#endif
// Run-3 switches are explicit stubs until their owners wire the implementation.
struct Config {const char* name;bool c1=false,c2=false,c3=false;
    bool strips=false,pageFix=false,quadCache=false;
    unsigned ioPriority=0x22;};
inline constexpr Config shipped{"shipped",true,true,true};  // C2 accepted by eye, user 2026-11-15
#if defined(HKPSP_PERF) || defined(HKPSP_CPU_ATTRIB)
#if defined(HKPSP_PERF) && !defined(HKPSP_PERF_SWEEP)
// Default perf build: only the shipped configuration (one 600-frame run on hardware).
// HKPSP_PERF_SWEEP restores the 11-configuration sweep below (package.ps1 -PerfSweep).
// The second slot is used only by the runtime room-ge-ab.cfg comparison.
#ifndef HKPSP_GPU_ATTRIB
inline constexpr std::array<Config,2> configs{{shipped,shipped}};
inline bool roomGeAB=false;
#else
inline constexpr std::array<Config,1> configs{{shipped}};
#endif
inline constexpr unsigned shippedIndex=0;
#else
inline constexpr std::array<Config,
#ifdef HKPSP_PERF
    11
#else
    9
#endif
    > configs{{{"baseline"},{"c1",true},{"c2",false,true},{"c3",false,false,true},shipped,
    {"strips",true,true,true,true},{"page_fix",true,true,true,false,true},
    {"quad_cache",true,true,true,false,false,true},{"all",true,true,true,true,true,true}
#ifdef HKPSP_PERF
    ,{"io_22",true,true,true,false,false,false,0x22},{"io_30",true,true,true,false,false,false,0x30}
#endif
    }};
inline constexpr unsigned shippedIndex=4;
#endif
inline unsigned configIndex=0;
inline unsigned configCount(){
#if defined(HKPSP_PERF) && !defined(HKPSP_PERF_SWEEP) && !defined(HKPSP_GPU_ATTRIB)
    return roomGeAB?2:1;
#else
    return configs.size();
#endif
}
inline const Config& configAt(unsigned index){
#if defined(HKPSP_PERF) && !defined(HKPSP_PERF_SWEEP) && !defined(HKPSP_GPU_ATTRIB)
    static constexpr std::array<Config,2> comparison{{{"room_exact",true,true,true},{"room_ge",true,true,true}}};
    if(roomGeAB)return comparison.at(index);
#endif
    return configs.at(index);
}
inline const Config& config(){return configAt(configIndex);}
#else
inline constexpr const Config& config(){return shipped;}
#endif
inline float focal=0;
}
// Detailed profile hooks disappear in the normal EBOOT. Page I/O counters live
// separately in page_residency and are retained in the shipped build.
#ifdef HKPSP_PERF
#include <algorithm>
#include <atomic>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace hkpsp::perf {
#ifdef HKPSP_GPU_ATTRIB
inline constexpr unsigned frames=150;
inline constexpr const char* directory="ms0:/PSP/GAME/HKPSPGPU/out/";
#else
inline constexpr unsigned frames=600;
inline constexpr const char* directory="ms0:/PSP/GAME/HKPSPPERF/out/";
#endif
enum Column { Input, Simulation, DrawList, Gu, Sync, Vblank, Total,
              Draws, Quads, Triangles, TextureBinds, BlendChanges, Clipping,
              SpatialChecks, SpatialRejects, RoomCandidates, OwnerChecks, OwnerRejects,
              RoomTests, RejectPass, RejectSplit, RejectAlpha, RejectClip, RejectOpacity, RejectFrustum, RoomQuads,
              SimHero, SimEnemies, SimFlow, SimDecor, SimAmbient, SimParticles,
              VblankDelta, PageLoads, PageOpen, PageRead, PageVerify, PageLoad,
              PageEvictions, ResidentBytes, BlockingMiss, TextureBytes,
              GeScene, GeGrade, GeHud, CpuScene, CpuGrade, CpuHud, ArenaBytes, LargestFree,
              AudioBytes, StreamUnderruns, TailUnderruns,
              NailHits, DecorCuts, DamageTaken, Pickups,
              AudioReads, AudioReadUs, AudioReadCpuUs, AudioWakeups, AudioCpuProbeErrors,
              GameRun, MixerRun, IoRun, IdleRun, KernelInterrupt, RuntimeExcess, RuntimeProbeErrors,
              SimulationRun, DrawListRun, GuRun, AudioSubmit, Bookkeeping, Swap,
              ProbeCost, ProbeCalls,
              PageOverflowFrames, PageGeWaits, PageGeWaitUs,
              ClipStorageUs, ClipTrianglesUs, ClipInitializedBytes,
              UploadPagesUs, UploadHeapUs, UploadBookkeepingUs, SubmitBatches, PagePrepareBatches,
              AudioOpens, AudioOpenUs, AudioOpenCpuUs, AudioCloses, AudioCloseUs, AudioCloseCpuUs,
              AudioSeeks, AudioSeekUs, AudioSeekCpuUs, AudioClipChanges,
              AudioRead8K, AudioRead16K, AudioRead32K, AudioRead64K, AudioReadLarge,
              AudioRingLow, AudioTailLow,
              PageVramBytes, PageVramFreeBytes, PagePoolSlots, PageFixedPages, PagePassEvictions, ClutLoads,
               GuStateCommands, TextureFlushes,
               SimSchedule, SimPhysicsCallbacks, SimContacts, SimCombat, SimSnapshot,
               SimWorld, SimInteractions, SimSession, SimObserve,
               SimBodies, SimColliderVisits, SimCombatTests, SimDecorTests, SimDecorParticles, SimAmbientSlots, SimDecorEmissionSlots,
               SimHashLo, SimHashHi, SimStateLeaves, SimDecorTerrainTests, SimDecorTerrainQueries,
                RoomCpuProjected, RoomRunsDrawn, RoomGeVertices, RoomVertexBytes, RoomAlphaFallback,
                RoomGeMode, GameHeapBytes, RoomSceneBytes,
                RoomGeMatrixLoads, RoomGeMatrixBytes, RoomGeDrawBytes, GuListBytes,
                RoomGeStreamVisits, RoomGeKeySearches, RoomGeOrderTests, RoomGeSpanVisits, Columns };
struct Row:std::array<unsigned,Columns> {std::string pageLoadedIds;};
inline std::array<Row,frames> rows{};
inline Row current{};
inline unsigned started=0,recorded=0,configStarted=0;
struct PageEvent {unsigned frame,page;bool eviction;};
inline std::array<PageEvent,8192> pageEvents{};
inline unsigned pageEventCount=0;
// Audio workers publish against the current render frame. Read only after join.
inline std::atomic<unsigned> audioFrame{0};
inline std::array<std::atomic<unsigned>,frames> audioBytes{},streamUnderruns{},tailUnderruns{};
inline std::array<std::atomic<unsigned>,frames> audioReads{},audioReadUs{},audioReadCpuUs{},audioWakeups{},audioCpuProbeErrors{};
inline std::array<std::array<std::atomic<unsigned>,AudioTailLow-AudioOpens+1>,frames> audioExtra{};
inline void audioAdd(Column column,unsigned value=1){
    audioExtra[audioFrame.load(std::memory_order_relaxed)][column-AudioOpens].fetch_add(value,std::memory_order_relaxed);
}
// Store value+1: zero means no observation in this frame, rather than an empty ring.
inline void audioLow(Column column,unsigned value){
    auto& low=audioExtra[audioFrame.load(std::memory_order_relaxed)][column-AudioOpens];
    unsigned previous=low.load(std::memory_order_relaxed);
    while((!previous||value+1<previous)&&!low.compare_exchange_weak(previous,value+1,std::memory_order_relaxed)){}
}
inline void audioOperation(Column column,unsigned us,unsigned cpuUs,bool probeError){
    const unsigned frame=audioFrame.load(std::memory_order_relaxed),index=column-AudioOpens;
    audioExtra[frame][index].fetch_add(1,std::memory_order_relaxed);
    audioExtra[frame][index+1].fetch_add(us,std::memory_order_relaxed);
    audioExtra[frame][index+2].fetch_add(cpuUs,std::memory_order_relaxed);
    audioCpuProbeErrors[frame].fetch_add(unsigned(probeError),std::memory_order_relaxed);
}
inline void audioRead(unsigned bytes,unsigned us,unsigned cpuUs,bool probeError=false){
    const unsigned frame=audioFrame.load(std::memory_order_relaxed);
    audioBytes[frame].fetch_add(bytes,std::memory_order_relaxed);
    audioReads[frame].fetch_add(1,std::memory_order_relaxed);
    audioReadUs[frame].fetch_add(us,std::memory_order_relaxed);
    audioReadCpuUs[frame].fetch_add(cpuUs,std::memory_order_relaxed);
    audioCpuProbeErrors[frame].fetch_add(unsigned(probeError),std::memory_order_relaxed);
    audioExtra[frame][(bytes<=8192?AudioRead8K:bytes<=16384?AudioRead16K:bytes<=32768?AudioRead32K:
        bytes<=65536?AudioRead64K:AudioReadLarge)-AudioOpens].fetch_add(1,std::memory_order_relaxed);
}
inline void audioWake(){audioWakeups[audioFrame.load(std::memory_order_relaxed)].fetch_add(1,std::memory_order_relaxed);}
inline void audioUnderrun(bool tail){(tail?tailUnderruns:streamUnderruns)[audioFrame.load(std::memory_order_relaxed)].fetch_add(1,std::memory_order_relaxed);}
// Called only after both workers join; exchange prevents leakage into the next config.
inline void finishAudio(){
    for(unsigned f=0;f<frames;++f){
        rows[f][AudioBytes]=audioBytes[f].exchange(0);
        rows[f][StreamUnderruns]=streamUnderruns[f].exchange(0);
        rows[f][TailUnderruns]=tailUnderruns[f].exchange(0);
        rows[f][AudioReads]=audioReads[f].exchange(0);
        rows[f][AudioReadUs]=audioReadUs[f].exchange(0);
        rows[f][AudioReadCpuUs]=audioReadCpuUs[f].exchange(0);
        rows[f][AudioWakeups]=audioWakeups[f].exchange(0);
        rows[f][AudioCpuProbeErrors]=audioCpuProbeErrors[f].exchange(0);
        for(unsigned column=AudioOpens;column<=AudioTailLow;++column){
            const unsigned value=audioExtra[f][column-AudioOpens].exchange(0);
            rows[f][column]=(column==AudioRingLow||column==AudioTailLow)?(value?value-1:0):value;
        }
    }
    audioFrame.store(0);
}
inline void pageEvent(unsigned page,bool eviction){
    if(pageEventCount==pageEvents.size())throw std::runtime_error("Perf page event overflow");
    pageEvents[pageEventCount++]={recorded,page,eviction};
}
// Implemented by the PSP adapter; the loop/draw list never call SDK functions.
unsigned now();
// SDK adapters only; cumulative run clocks are microseconds, not 333 MHz cycles.
inline int gameThread=-1,mixerThread=-1,ioThread=-1;
unsigned runClock(int thread);
std::array<unsigned,4> runtime();
inline std::array<unsigned,4> runtimeStart{};
inline unsigned preloadBytes=0,inputRun=0;
#ifndef HKPSP_GPU_ATTRIB
void writeWorkSummary(FILE*);
#endif
inline void begin(){current={};inputRun=0;audioFrame.store(recorded,std::memory_order_relaxed);started=now();runtimeStart=runtime();}
inline void end(){
    const auto bookkeepingStart=now();
    const auto last=runtime();
    current[Bookkeeping]+=now()-bookkeepingStart;
    current[Total]=now()-started;
    unsigned sum=0;
    for(unsigned i=0;i<4;++i){current[GameRun+i]=last[i]-runtimeStart[i];sum+=current[GameRun+i];}
    current[KernelInterrupt]=current[Total]>sum?current[Total]-sum:0;
    current[RuntimeExcess]=sum>current[Total]?sum-current[Total]:0;
    if(recorded>=frames)throw std::runtime_error("Perf frame overflow");
    rows[recorded++]=current;
}
inline FILE* open(const char* name){
    const auto path=std::string(directory)+name;
    FILE* f=std::fopen(path.c_str(),"wb");
    if(!f)throw std::runtime_error("Cannot write "+path);
    return f;
}
inline void close(FILE* f){
    const bool failed=std::ferror(f)!=0;
    const int result=std::fclose(f);
    if(failed||result)throw std::runtime_error("Perf output write failed");
}
inline constexpr const char* names[]={"input_us","simulation_us","draw_list_us","gu_us","sync_us",
        "vblank_us","total_us","draws_considered","quads","triangles","texture_binds","blend_changes","clipping_us",
        "spatial_checks","spatial_rejects","room_candidates","owner_checks","owner_rejects",
        "room_tests","reject_pass","reject_split","reject_alpha","reject_clip","reject_opacity","reject_frustum","room_quads",
        "sim_hero_us","sim_enemies_us","sim_flow_us","sim_decor_us","sim_ambient_us","sim_particles_us",
        "vblank_delta","page_loads","page_open_us","page_read_us","page_verify_us","page_load_us",
        "page_evictions","resident_bytes","blocking_miss","texture_read_bytes",
        "ge_scene_us","ge_grade_us","ge_hud_us","cpu_scene_us","cpu_grade_us","cpu_hud_us","arena_bytes","largest_free_block",
        "audio_read_bytes","audio_stream_underruns","audio_tail_underruns",
        "event_nail_hits","event_decor_cuts","event_damage_taken","event_pickup_geo",
        "audio_read_calls","audio_read_us","audio_read_cpu_us","audio_pump_wakeups","audio_cpu_probe_errors",
        "game_run_us","mixer_run_us","audio_io_run_us","idle_run_us","kernel_interrupt_us","runtime_excess_us","runtime_probe_errors",
        "simulation_run_us","draw_list_run_us","gu_run_us","audio_submit_us","perf_bookkeeping_us","swap_us",
        "runtime_probe_us","runtime_probe_calls",
        "page_overflow_frames","page_ge_waits","page_ge_wait_us",
        "clip_storage_us","clip_triangles_us","clip_initialized_bytes",
        "upload_pages_us","upload_heap_us","upload_bookkeeping_us","submit_batches","page_prepare_batches",
        "audio_open_calls","audio_open_us","audio_open_cpu_us","audio_close_calls","audio_close_us","audio_close_cpu_us",
        "audio_seek_calls","audio_seek_us","audio_seek_cpu_us","audio_clip_changes",
        "audio_reads_le_8192","audio_reads_le_16384","audio_reads_le_32768","audio_reads_le_65536","audio_reads_gt_65536",
        "audio_ring_low_frames","audio_tail_low_pages",
        "page_vram_bytes","page_vram_free_bytes","page_pool_slots","page_fixed_pages","page_pass_evictions","clut_loads",
        "gu_state_commands","texture_flushes",
        "sim_schedule_us","sim_physics_callbacks_us","sim_contacts_us","sim_combat_us","sim_snapshot_us",
        "sim_world_us","sim_interactions_us","sim_session_us","sim_observe_us",
        "sim_bodies","sim_collider_visits","sim_combat_tests","sim_decor_tests","sim_decor_particles","sim_ambient_slots","sim_decor_emission_slots",
        "sim_state_hash_lo","sim_state_hash_hi","sim_state_leaves","sim_decor_terrain_tests","sim_decor_terrain_queries",
        "room_cpu_projected","room_runs_drawn","room_ge_vertices","room_vertex_bytes","room_alpha_fallback",
        "room_ge_mode","game_heap_bytes","room_scene_bytes",
        "room_ge_matrix_loads","room_ge_matrix_bytes","room_ge_draw_bytes","gu_list_bytes",
        "room_ge_stream_visits","room_ge_key_searches","room_ge_order_tests","room_ge_span_visits"};
    static_assert(sizeof(names)/sizeof(*names)==Columns,"Perf column names drifted");
inline void write(){
    if(recorded!=frames)throw std::runtime_error("Incomplete perf run");
    FILE* csv=open("perf.csv");
    std::fprintf(csv,"frame");
    for(const auto* name:names)std::fprintf(csv,",%s",name);
    std::fprintf(csv,",page_loaded_ids\n");
    for(unsigned frame=0;frame<frames;++frame){
        std::fprintf(csv,"%u",frame);
        for(const auto value:rows[frame])std::fprintf(csv,",%u",value);
        std::fprintf(csv,",%s\n",rows[frame].pageLoadedIds.c_str());
    }
    close(csv);
    FILE* summary=open("summary.txt");
    std::fprintf(summary,"frames=%u\nclock_cpu_mhz=333\nclock_bus_mhz=166\n"
        "Timing source: sceKernelGetSystemTimeLow; PPSSPP is shape only, never hardware acceptance.\n"
        "All 600 frames included; startup texture upload is in frame 0 gu_us.\n"
        "draw_list_us includes clipping_us (a subset); gu_us excludes clipping.\n"
        "draws_considered counts index leaf/mutable bounds checks, admitted room-record visits and hero.\n"
        "texture_binds/blend_changes count emitted state calls, including redundant calls.\n"
        "Total spans draw preparation, submission, waits, swap and both simulation steps.\n"
        "Input is tape sampling inside simulation; simulation_us excludes that input time.\n"
        "Summary/CSV writes occur after measurement; timer overhead remains included.\n"
        "column,mean,p50,p95,max\n",frames);
    double total=0;
    for(unsigned column=0;column<Columns;++column){
        std::array<unsigned,frames> sorted{};double sum=0;
        for(unsigned frame=0;frame<frames;++frame){sorted[frame]=rows[frame][column];sum+=sorted[frame];}
        std::sort(sorted.begin(),sorted.end());
        // Nearest-rank percentiles, including startup; all arithmetic is post-run.
        std::fprintf(summary,"%s,%.3f,%u,%u,%u\n",names[column],sum/frames,
            sorted[(frames*50+99)/100-1],sorted[(frames*95+99)/100-1],sorted.back());
        if(column==Total)total=sum;
    }
    std::fprintf(summary,"fps=%.6f\n",total>0?frames*1000000./total:0.);
#ifndef HKPSP_GPU_ATTRIB
    writeWorkSummary(summary);
#endif
    close(summary);
}
}
#define HKPSP_PROFILE(...) __VA_ARGS__
#else
#define HKPSP_PROFILE(...)
#endif

#ifdef HKPSP_PERF
namespace hkpsp::cpu {
enum Phase { Prepare, Plants, Ambient, Decor, Particles, Motion, Index, Sort, Filter, Projection, Lights, Actors, Grade, Hud, OwnerTable, HudText, Other, Clip,
             Upload, Commands, Finish, Phases, Far=Other, Near=Other };
inline constexpr const char* phaseNames[]={"prepare_us","plants_us","ambient_us","decor_us","particles_us","motion_us","index_us","sort_us","filter_us","projection_us",
    "lights_us","actors_us","grade_build_us","hud_us","owner_table_us","hud_text_us","other_us","clip_us","upload_us","commands_us","finish_us"};
using Times=std::array<unsigned,Phases>;
inline Times times{};
inline Times runTimes{};
inline unsigned drawCalls=0;
// Coarse, exclusive scopes. Nested phases pause their parent; never clock a draw.
inline int active=-1;
inline unsigned changed=0;
inline unsigned changedRun=0;
inline unsigned change(int phase){
    const auto run=perf::runClock(perf::gameThread);const auto t=perf::now();
    if(active>=0){times[active]+=t-changed;runTimes[active]+=run-changedRun;}
    active=phase;changed=t;changedRun=run;return t;
}
struct Scope {
    int parent;
    explicit Scope(Phase phase):parent(active){change(phase);}
    ~Scope(){change(parent);}
};
#ifndef HKPSP_GPU_ATTRIB
struct Frame {perf::Row row{};Times phases{};unsigned calls=0;};
inline std::array<std::array<Frame,perf::frames>,configs.size()> records{};
inline std::array<unsigned,configs.size()> configUs{};
inline void finishConfig(){
    // runGameConfig has stopped and joined both audio workers here.
    perf::finishAudio();
    const auto path=std::string(perf::directory)+"page-events.csv";
    FILE* events=std::fopen(path.c_str(),configIndex?"ab":"wb");
    if(!events)throw std::runtime_error("Cannot write page events");
    if(!configIndex)std::fprintf(events,"config,frame,page,event\n");
    for(unsigned i=0;i<perf::pageEventCount;++i){const auto& e=perf::pageEvents[i];
        std::fprintf(events,"%s,%u,%u,%s\n",config().name,e.frame,e.page,e.eviction?"evict":"load");}
    perf::close(events);perf::pageEventCount=0;
    if(perf::recorded!=perf::frames)throw std::runtime_error("Incomplete CPU config");
    for(unsigned f=0;f<perf::frames;++f)records[configIndex][f].row=perf::rows[f];
    perf::recorded=0;
    configUs[configIndex]=perf::now()-perf::configStarted;
}
inline void write(){
    FILE* csv=perf::open("cpu.csv");
    FILE* manifest=perf::open("run3-configs.csv");
    std::fprintf(manifest,"config,strips_requested,page_fix_requested,quad_cache_requested,implementation,audio_io_priority,no_streaming_reads,preload_pcm_bytes\n");
    for(unsigned i=0;i<configCount();++i){const auto& c=configAt(i);
        std::fprintf(manifest,"%s,%u,%u,%u,stub,%u,0,%u\n",c.name,unsigned(c.strips),unsigned(c.pageFix),unsigned(c.quadCache),c.ioPriority,perf::preloadBytes);}
    perf::close(manifest);
    std::fprintf(csv,"config,frame,draw_list_us,gu_us,total_us,quads,triangles,texture_binds,blend_changes,draw_calls");
    for(const auto* name:phaseNames)std::fprintf(csv,",%s",name);
    std::fprintf(csv,",sync_us,vblank_us,simulation_us,ge_wait_us,input_us");
    for(unsigned i=perf::SpatialChecks;i<perf::Columns;++i)std::fprintf(csv,",%s",perf::names[i]);
    std::fprintf(csv,",page_loaded_ids\n");
    FILE* summary=perf::open("cpu-summary.csv");
    std::fprintf(summary,"config,frames,mean_draw_list_ms,p95_draw_list_ms,mean_gu_ms,mean_total_ms,mean_draw_calls");
    for(const auto* name:phaseNames)std::fprintf(summary,",mean_%s",name);
    std::fprintf(summary,",mean_simulation_us,mean_input_us");
    for(unsigned i=perf::SpatialChecks;i<perf::Columns;++i)std::fprintf(summary,",mean_%s",perf::names[i]);
    std::fprintf(summary,"\n");
    for(unsigned c=0;c<configCount();++c){
        std::array<unsigned,perf::frames> sorted{};double dl=0,gu=0,total=0,calls=0;
        std::array<double,Phases> sums{};
        std::array<double,perf::Columns> details{};
        for(unsigned f=0;f<perf::frames;++f){
            const auto& r=records[c][f];const auto& p=r.row;
            std::fprintf(csv,"%s,%u,%u,%u,%u,%u,%u,%u,%u,%u",configAt(c).name,f,p[perf::DrawList],p[perf::Gu],p[perf::Total],
                p[perf::Quads],p[perf::Triangles],p[perf::TextureBinds],p[perf::BlendChanges],r.calls);
            for(unsigned i=0;i<Phases;++i){std::fprintf(csv,",%u",r.phases[i]);sums[i]+=r.phases[i];}
            // Residual wait after the last submitted pass, including list finish.
            std::fprintf(csv,",%u,%u,%u,%u,%u",p[perf::Sync],p[perf::Vblank],p[perf::Simulation],p[perf::Sync]+r.phases[Finish],p[perf::Input]);
            details[perf::Simulation]+=p[perf::Simulation];
            details[perf::Input]+=p[perf::Input];
            for(unsigned i=perf::SpatialChecks;i<perf::Columns;++i){std::fprintf(csv,",%u",p[i]);details[i]+=p[i];}
            std::fprintf(csv,",%s\n",p.pageLoadedIds.c_str());sorted[f]=p[perf::DrawList];dl+=sorted[f];gu+=p[perf::Gu];total+=p[perf::Total];calls+=r.calls;
        }
        std::sort(sorted.begin(),sorted.end());
        std::fprintf(summary,"%s,%u,%.6f,%.6f,%.6f,%.6f,%.6f",configAt(c).name,perf::frames,
            dl/perf::frames/1000.,sorted[(perf::frames*95+99)/100-1]/1000.,gu/perf::frames/1000.,total/perf::frames/1000.,calls/perf::frames);
        for(const auto sum:sums)std::fprintf(summary,",%.6f",sum/perf::frames);
        std::fprintf(summary,",%.6f,%.6f",details[perf::Simulation]/perf::frames,details[perf::Input]/perf::frames);
        for(unsigned i=perf::SpatialChecks;i<perf::Columns;++i)std::fprintf(summary,",%.6f",details[i]/perf::frames);
        std::fprintf(summary,"\n");
    }
    perf::close(csv);perf::close(summary);
    FILE* seconds=perf::open("config-seconds.csv");
    std::fprintf(seconds,"config,seconds\n");
    for(unsigned c=0;c<configCount();++c)std::fprintf(seconds,"%s,%.6f\n",configAt(c).name,configUs[c]/1000000.);
    perf::close(seconds);
    FILE* done=perf::open("cpu-done.txt");
    std::fprintf(done,"configs=%u\nframes_per_config=%u\nframes=%u\ncontrol_steps=%u\n",
        configCount(),perf::frames,configCount()*perf::frames,configCount()*perf::frames*2);
    perf::close(done);
}
#endif
}
#define HKPSP_PHASE(phase,...) do {hkpsp::cpu::Scope phaseScope(hkpsp::cpu::phase); __VA_ARGS__; } while(false)
#else
#define HKPSP_PHASE(phase,...) __VA_ARGS__
#endif
