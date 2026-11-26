#pragma once
#include <3ds.h>
#include <citro2d.h>
#include <cstddef>
#include <cstdio>

namespace hk3ds {
// Empty adapters let call sites disappear without allocating observer storage.
class FrameProfile {
public:
    enum Stage { Input, Logic, Save, Audio, SectorWait, Metadata, Page, Draw,
                 FrameBegin, FrameEnd, Diagnostics, Observer, LoadWait, GpuWait, Count };
    struct Scope { template<class... T> explicit Scope(T&&...) {} };
    using Detail=Scope;
    using UploadScope=Scope;
    constexpr bool enabled() const {return false;}
    constexpr u64 tick() const {return 0;}
    template<class... T> void begin(T&&...) {}
    template<class... T> void simulation(T&&...) {}
    template<class... T> void memory(T&&...) {}
    template<class... T> void page(T&&...) {}
    template<class... T> void uploaded(T&&...) {}
    template<class... T> void deferred(T&&...) {}
    template<class... T> void event(T&&...) {}
    template<class... T> void worker(T&&...) {}
    template<class... T> void saved(T&&...) {}
    template<class... T> void obsoleteWait(T&&...) {}
    template<class... T> void cache(T&&...) {}
    template<class... T> void commit(T&&...) {}
    template<class... T> void next(T&&...) {}
    template<class... T> bool finish(T&&...) {return true;}
};
inline FrameProfile frameProfile;
class AudioEvidence {
public:
    constexpr bool enabled() const {return false;}
    template<class... T> void begin(T&&...) {}
    template<class... T> void startHardwareSampling(T&&...) {}
    template<class... T> void stopHardwareSampling(T&&...) {}
    template<class... T> void freeze(T&&...) {}
    template<class... T> void outputStopped(T&&...) {}
    template<class... T> bool writeOutputStop(T&&...) const {return true;}
    template<class... T> void beforeUpdate(T&&...) {}
    template<class... T> void configure(T&&...) {}
    template<class... T> void sampleGameplay(T&&...) {}
    template<class... T> void afterUpdate(T&&...) {}
    template<class... T> void sampleScene(T&&...) {}
    template<class... T> bool write(T&&...) const {return true;}
};
inline AudioEvidence audioEvidence;
class ConsoleMetrics {
public:
    template<class... T> void begin(T&&...) {}
    template<class... T> void sample(T&&...) {}
    template<class... T> bool finish(T&&...) {return true;}
};
}
