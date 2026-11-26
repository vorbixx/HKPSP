#pragma once
#include <cstdio>
#include <cstdint>

namespace hk3ds {
struct CrashBreadcrumb {
    char build[96]{};
    char scene[64]{};
    char gate[64]{};
    char stage[96]{};
    char reason[96]{};
    std::uint32_t linearFree=0;
    std::uint32_t heapFree=0;
    char revision[64]="unknown";
    bool diagnostics=false;

    void beginSession() {
        set(scene,sizeof(scene),"Tutorial_01");
        set(gate,sizeof(gate),"startup");
        set(stage,sizeof(stage),"startup");
        reason[0]=0;
    }
    void set(char* destination, std::size_t size, const char* value) {
        std::snprintf(destination,size,"%s",value?value:"");
    }
    bool writeFields(FILE* file) const {
        return std::fprintf(file,"build=%s\nscene=%s\ngate=%s\nstage=%s\nlinear_free=%lu\nheap_free=%lu\nreason=%s\nrevision=%s\ndiagnostics=%u\n",
            build,scene,gate,stage,static_cast<unsigned long>(linearFree),
            static_cast<unsigned long>(heapFree),reason,revision,unsigned(diagnostics))>=0;
    }
    bool write(const char* path) const {
        auto* file=std::fopen(path,"w");
        if(!file)return false;
        const bool ok=writeFields(file);
        return std::fclose(file)==0&&ok;
    }
};
}
