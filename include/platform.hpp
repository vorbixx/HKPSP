#pragma once
// libctru platform adapter; no game rules depend on the CPU operating mode.
#include <3ds.h>
#include "graphics_profile.hpp"
#include <cstdio>

namespace hk3ds {
struct PlatformPerformance {
    Result modelResult=0;
    bool new3ds=false, speedupRequested=false;
    unsigned memoryMode=0;
    bool simulationWorkerRequested=false,simulationWorkerEnabled=false;
    unsigned simulationSnapshotBytes=0;
};

inline PlatformPerformance configureNew3dsPerformance() {
    PlatformPerformance mode;
    mode.modelResult=APT_CheckNew3DS(&mode.new3ds);
    // libctru requests clock + L2 (PTMSYSM flags 3), remembers this preference
    // across APT sleep/resume, and safely handles systems without N3DS speedup.
    if(mode.new3ds){osSetSpeedupEnable(true);mode.speedupRequested=true;}
    mode.memoryMode=osGetApplicationMemType();
    return mode;
}

inline bool writePlatformPerformance(std::FILE* file,const PlatformPerformance& mode) {
    // osSetSpeedupEnable is void: record the request, never an observed clock.
    return std::fprintf(file," new_3ds=%u model_result=0x%08lX speedup_requested=%u app_memtype=%u simulation_worker_requested=%u simulation_worker_enabled=%u simulation_worker_core=2 simulation_snapshot_bytes=%u graphics_profile=%u room_texture_budget=%u\n",
        unsigned(mode.new3ds),static_cast<unsigned long>(static_cast<u32>(mode.modelResult)),
        unsigned(mode.speedupRequested),mode.memoryMode,unsigned(mode.simulationWorkerRequested),
        unsigned(mode.simulationWorkerEnabled),mode.simulationSnapshotBytes,unsigned(!graphicsProfile().postprocessing),unsigned(graphicsProfile().roomTextureBytes))>0;
}
}
