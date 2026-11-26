#pragma once
#include "build_config.hpp"
#include <atomic>
#include <cerrno>
#include <cstdio>

namespace hk3ds {
// Independent of HK3DS_DIAGNOSTICS: this never enables automation or gameplay cheats.
class RuntimeDiagnostics {
    std::atomic<bool> enabled_{false};
public:
    static constexpr const char* marker="sdmc:/3ds/HK3DS/diagnostics.on";
    bool enabled() const {return enabled_.load(std::memory_order_relaxed);}
    void load(const char* path=marker) {
        auto* file=std::fopen(path,"rb");
        enabled_.store(file!=nullptr,std::memory_order_relaxed);
        if(file)std::fclose(file);
    }
    // Keep the previous state if the SD write fails, so the confirmation is truthful.
    bool set(bool on,const char* path=marker) {
        if(on){
            auto* file=std::fopen(path,"wb");
            if(!file)return false;
            if(std::fclose(file)!=0)return false;
        }else if(std::remove(path)!=0&&errno!=ENOENT)return false;
        enabled_.store(on,std::memory_order_relaxed);
        return true;
    }
};
inline RuntimeDiagnostics runtimeDiagnostics;
// Existing Diagnostic automation flags remain available without enabling the overlay.
inline bool diagnosticLogRequested(const char* flag) {
    if(runtimeDiagnostics.enabled())return true;
    if constexpr(diagnosticsEnabled){
        if(auto* file=std::fopen(flag,"rb")){std::fclose(file);return true;}
    }
    return false;
}
}
