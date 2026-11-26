#pragma once
#include <cstddef>
#ifdef __3DS__
#include <3ds.h>
#endif

namespace hk3ds {
// One bounded CPU job. GPU, HID, audio and filesystem adapters stay on main.
class CpuWorker {
public:
    using Job=void(*)(void*);
    static constexpr std::size_t stackBytes=128*1024;
    CpuWorker()=default;
    CpuWorker(const CpuWorker&)=delete;
    CpuWorker& operator=(const CpuWorker&)=delete;
    ~CpuWorker(){stop();}
    bool start(bool requested);
    bool asynchronous() const;
    void submit(Job,void*);
    void wait();
    void stop();
private:
    bool pending_=false;
#ifdef __3DS__
    Thread thread_=nullptr;
    LightEvent request_{},done_{};
    LightLock lock_{};
    Job job_=nullptr;
    void* context_=nullptr;
    bool stopping_=false;
    static void entry(void*);
#endif
};
}
