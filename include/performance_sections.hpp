#pragma once
#include "build_config.hpp"
#include "room_owner_profile.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <limits>
#include <cmath>

namespace hk3ds {
// Nested CPU observations: never add these to the independent frame stages.
enum class PerformanceSection : unsigned {
    DecorUpdate, DecorPresent, AmbientUpdate, AmbientPresent, MergeSort,
    RoomSubmit, BackgroundEffects, Bloom, CameraColor, LightComposite, Submission,
    GpuBackground, GpuBlur0, GpuBlur1, GpuBlur2, GpuBlur3, GpuVibrancy,
    GpuScene, GpuGrabCopy, GpuLightClear, GpuLightSprite, GpuLightComposite,
    GpuBloomExtract, GpuBloomBlur, GpuBloomAdd, GpuCameraColor, GpuDisplay,
    PreviousGpuFrame, GpuOwnerPrepare, GpuOwner, RoomFilter, RoomProjection, RoomState, RoomC2D,
    GpuFogHaze, GpuActors, GpuHud,
    CpuBackground, CpuBehindHero, CpuActors, CpuForeground, CpuPresent, CpuHud, CpuKick,
    GpuKickWait, SimHero, SimEnemies, SimFlow, SimParticles,
    SimSchedule, SimPhysicsCallbacks, SimContacts, SimCombat, SimSnapshot,
    SimWorld, SimInteractions, SimSession, SimObserve, Count
};
inline constexpr unsigned performanceSectionCount=unsigned(PerformanceSection::Count);
inline constexpr unsigned gpuProfileStride=8;
inline constexpr const char* performanceSectionNames[]={
    "decor_update_ms","decor_present_ms","ambient_update_ms","ambient_present_ms",
    "merge_sort_ms","room_submit_ms","background_effects_ms","bloom_ms",
    "camera_color_ms","light_composite_ms","submission_ms",
    "gpu_background_ms","gpu_blur0_ms","gpu_blur1_ms","gpu_blur2_ms","gpu_blur3_ms",
    "gpu_vibrancy_ms","gpu_scene_ms","gpu_grab_copy_ms","gpu_light_clear_ms",
    "gpu_light_sprite_ms","gpu_light_composite_ms","gpu_bloom_extract_ms",
    "gpu_bloom_blur_ms","gpu_bloom_add_ms","gpu_camera_color_ms","gpu_display_ms",
    "previous_gpu_frame_ms","gpu_owner_prepare_ms","gpu_owner_ms",
    "room_filter_sample_ms","room_projection_sample_ms","room_state_sample_ms",
    "room_c2d_sample_ms", "gpu_fog_haze_ms", "gpu_actors_ms", "gpu_hud_ms",
    "cpu_background_ms", "cpu_behind_hero_ms", "cpu_actors_ms", "cpu_foreground_ms",
    "cpu_present_ms", "cpu_hud_ms", "cpu_kick_ms", "gpu_kick_wait_ms",
    "sim_hero_ms", "sim_enemies_ms", "sim_flow_ms", "sim_particles_ms",
    "sim_schedule_ms", "sim_physics_callbacks_ms", "sim_contacts_ms", "sim_combat_ms",
    "sim_snapshot_ms", "sim_world_ms", "sim_interactions_ms", "sim_session_ms", "sim_observe_ms"
};
enum class SimulationWork : unsigned { Bodies, ColliderVisits, CombatTests, DecorTests, DecorParticles, AmbientSlots, DecorEmissionSlots, DecorTerrainTests, DecorTerrainQueries, Count };
struct PerformanceObservation {
    using Ticks=std::array<std::uint32_t,performanceSectionCount>;
    struct Coverage {
        std::uint32_t quads=0,backgroundPixels=0,foregroundPixels=0;
        std::uint32_t backgroundRefreshes=0,backgroundReuses=0;
        std::uint32_t gpuKicks=0;
        float backgroundCameraX=0,backgroundCameraY=0;
    };
    Ticks* row=nullptr;
    Coverage* coverage=nullptr;
    std::uint64_t (*clock)()=nullptr;
#if defined(HKPSP_PERF) || defined(HKPSP_SIM_TRACE)
    // Sum of exclusive simulation observations; parent scopes subtract children.
    std::uint64_t simulationAccounted=0;
    using Work=std::array<std::uint32_t,unsigned(SimulationWork::Count)>;
    Work* simulationWork=nullptr;
#endif
    // Presentation-only ablations retain simulation state and RNG progression.
    enum Feature : unsigned { Decor=1, Ambient=2, Blur=4, Color=8, Bloom=16, RoomSubmission=32, Lights=64 };
    static constexpr unsigned allFeatures=Decor|Ambient|Blur|Color|Bloom|RoomSubmission|Lights;
    unsigned disabled=0;
    // Diagnostic opt-in: serialize GPU work; these are not normal frame timings.
    bool serializedGpu=false;
    // Full-route pass attribution without owner isolation (mask bit 28).
    bool gpuStagesOnly=false;
    PerformanceSection gpuCategory=PerformanceSection::GpuBackground;
    bool gpuFrameSelected=false;
    bool cpuProfileEnabled=false,cpuFrameSelected=false;
    unsigned gpuProfileFailures=0;
    RoomOwnerProfile owners;
    unsigned submittedQuads=0,identityQuads=0,flatQuads=0;
    bool on(Feature feature) const {return !diagnosticsEnabled||!(disabled&feature);}
};
// Simulation and renderer write separate observations, merged after publication.
inline thread_local PerformanceObservation performanceObservation;
inline void observeSimulationWork(SimulationWork column,std::size_t count=1)
{
#if defined(HKPSP_PERF) || defined(HKPSP_SIM_TRACE)
    if(auto* work=performanceObservation.simulationWork){
        auto& value=(*work)[unsigned(column)];
        value+=std::uint32_t(std::min<std::size_t>(std::numeric_limits<std::uint32_t>::max()-value,count));
    }
#else
    (void)column;(void)count;
#endif
}
#if defined(HKPSP_PERF) || defined(HKPSP_SIM_TRACE)
inline bool simulationSection(PerformanceSection section)
{
    return section==PerformanceSection::DecorUpdate||section==PerformanceSection::AmbientUpdate||
        (section>=PerformanceSection::SimHero&&section<PerformanceSection::Count);
}
inline void recordSimulationSection(PerformanceSection section,std::uint64_t start,std::uint64_t accounted)
{
    auto& observation=performanceObservation;
    const auto elapsed=observation.clock()-start;
    const auto nested=observation.simulationAccounted-accounted;
    const auto exclusive=elapsed-std::min(elapsed,nested);
    auto& value=(*observation.row)[unsigned(section)];
    value=std::uint32_t(std::min<std::uint64_t>(std::numeric_limits<std::uint32_t>::max(),
        std::uint64_t(value)+exclusive));
    observation.simulationAccounted+=exclusive;
}
#endif
// PSP perf only. Accounts for the work between existing fine scopes, including
// early returns, without adding nested durations twice. No shipped clock calls.
struct SimulationScope {
#if defined(HKPSP_PERF) || defined(HKPSP_SIM_TRACE)
    PerformanceSection section;
    std::uint64_t start=0,accounted=0;
    bool active=false;
    explicit SimulationScope(PerformanceSection s):section(s){begin();}
    SimulationScope(const SimulationScope&)=delete;
    SimulationScope& operator=(const SimulationScope&)=delete;
    void begin(){
        if(performanceObservation.row){
            start=performanceObservation.clock();accounted=performanceObservation.simulationAccounted;
            active=true;
        }
    }
    void finish(){
        if(active){recordSimulationSection(section,start,accounted);active=false;}
    }
    void next(PerformanceSection s){finish();section=s;begin();}
    ~SimulationScope(){finish();}
#else
    explicit SimulationScope(PerformanceSection){}
    void finish(){}
    void next(PerformanceSection){}
#endif
};
inline void observeBackgroundCache(bool reused,float cameraX,float cameraY)
{
    if constexpr(!diagnosticsEnabled)return;
    auto* coverage=performanceObservation.coverage;if(!coverage)return;
    ++(reused?coverage->backgroundReuses:coverage->backgroundRefreshes);
    coverage->backgroundCameraX=cameraX;coverage->backgroundCameraY=cameraY;
}
// CPU-projected rectangles before the optional C2D view. Transparent texels
// and tilted triangles make this coverage an estimate, not a fragment counter.
inline void observeRoomCoverage(float left,float top,float right,float bottom,
                                unsigned width,unsigned height)
{
    if constexpr(!diagnosticsEnabled)return;
    auto* coverage=performanceObservation.coverage;if(!coverage)return;
    ++coverage->quads;
    const auto extent=[](float lo,float hi,unsigned size){
        return std::max(0.f,std::min(float(size),std::ceil(hi))-
                              std::max(0.f,std::floor(lo)));
    };
    const auto area=std::uint64_t(extent(left,right,width)*extent(top,bottom,height));
    auto& pixels=width!=400?coverage->backgroundPixels:coverage->foregroundPixels;
    pixels=std::uint32_t(std::min<std::uint64_t>(std::numeric_limits<std::uint32_t>::max(),
                                             std::uint64_t(pixels)+area));
}
struct PerformanceScope {
#if HK3DS_DIAGNOSTICS
    PerformanceSection section;
    std::uint64_t start;
    bool active=performanceObservation.row!=nullptr;
#if defined(HKPSP_PERF) || defined(HKPSP_SIM_TRACE)
    std::uint64_t accounted=performanceObservation.simulationAccounted;
#endif
    explicit PerformanceScope(PerformanceSection s):section(s),
        start(performanceObservation.row?performanceObservation.clock():0){}
    ~PerformanceScope(){
        if(!active)return;
#if defined(HKPSP_PERF) || defined(HKPSP_SIM_TRACE)
        if(simulationSection(section)){recordSimulationSection(section,start,accounted);return;}
#endif
        auto& value=(*performanceObservation.row)[unsigned(section)];
        value=std::uint32_t(std::min<std::uint64_t>(std::numeric_limits<std::uint32_t>::max(),
            std::uint64_t(value)+performanceObservation.clock()-start));
    }
#else
    explicit PerformanceScope(PerformanceSection) {}
#endif
};
// Disjoint CPU pass costs within draw_ms; nested room/effect scopes stay separate.
struct DrawCpuLap {
#if HK3DS_DIAGNOSTICS
    std::uint64_t start=performanceObservation.row?performanceObservation.clock():0;
    void mark(PerformanceSection section) {
        if(!start)return;
        const auto now=performanceObservation.clock();
        auto& value=(*performanceObservation.row)[unsigned(section)];
        value=std::uint32_t(std::min<std::uint64_t>(std::uint64_t(value)+now-start,
            std::numeric_limits<std::uint32_t>::max()));
        start=now;
    }
#else
    void mark(PerformanceSection) {}
#endif
};
}

namespace hk3ds {
// Sparse CPU detail shares the existing eighth-frame cadence. These observations
// are nested in RoomSubmit; exclude sampled frames from normal A/B frame means.
struct RoomCpuLap {
#if HK3DS_DIAGNOSTICS
    PerformanceSection section=PerformanceSection::RoomFilter;
    std::uint64_t start=0;
    RoomCpuLap() {
        const auto& o=performanceObservation;
        if(o.row&&o.cpuFrameSelected&&!o.serializedGpu)start=o.clock();
    }
    void mark(PerformanceSection next) {
        if(!start)return;
        const auto now=performanceObservation.clock();
        auto& value=(*performanceObservation.row)[unsigned(section)];
        value=std::uint32_t(std::min<std::uint64_t>(std::uint64_t(value)+now-start,
            std::numeric_limits<std::uint32_t>::max()));
        section=next;start=now;
    }
    ~RoomCpuLap(){mark(section);}
#else
    void mark(PerformanceSection) {}
#endif
};
}
