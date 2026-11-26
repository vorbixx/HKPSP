#include "cpu_worker.hpp"
#include <cassert>

namespace hk3ds {
bool CpuWorker::start(bool requested) {
    assert(!pending_);
#ifdef __3DS__
    if(thread_)return true;
    if(!requested)return false;
    LightLock_Init(&lock_);
    LightEvent_Init(&request_,RESET_ONESHOT);
    LightEvent_Init(&done_,RESET_ONESHOT);
    stopping_=false;
    s32 priority=0x30;
    svcGetThreadPriority(&priority,CUR_THREAD_HANDLE);
    // APT_SetAppCpuTimeLimit grants time on system core 1, not core 2.
    // Core 2 additionally requires the loader's New 3DS kernel permission.
    thread_=threadCreate(entry,this,stackBytes,priority,2,false);
    return thread_!=nullptr;
#else
    (void)requested;
    return false;
#endif
}
bool CpuWorker::asynchronous() const {
#ifdef __3DS__
    return thread_!=nullptr;
#else
    return false;
#endif
}
void CpuWorker::submit(Job job,void* context) {
    assert(job&&!pending_);
    pending_=true;
#ifdef __3DS__
    if(thread_){
        LightLock_Lock(&lock_);job_=job;context_=context;LightLock_Unlock(&lock_);
        LightEvent_Signal(&request_);
        return;
    }
#endif
    job(context);
}
void CpuWorker::wait() {
    if(!pending_)return;
#ifdef __3DS__
    if(thread_){
        LightEvent_Wait(&done_);
        // Pair the worker's publication with an acquire before reading state.
        LightLock_Lock(&lock_);LightLock_Unlock(&lock_);
    }
#endif
    pending_=false;
}
void CpuWorker::stop() {
    wait();
#ifdef __3DS__
    if(!thread_)return;
    LightLock_Lock(&lock_);stopping_=true;LightLock_Unlock(&lock_);
    LightEvent_Signal(&request_);
    threadJoin(thread_,U64_MAX);threadFree(thread_);thread_=nullptr;
#endif
}
#ifdef __3DS__
void CpuWorker::entry(void* context) {
    auto& self=*static_cast<CpuWorker*>(context);
    for(;;){
        LightEvent_Wait(&self.request_);
        LightLock_Lock(&self.lock_);
        const bool stopping=self.stopping_;
        const auto job=self.job_;auto* argument=self.context_;
        LightLock_Unlock(&self.lock_);
        if(stopping)return;
        job(argument);
        LightLock_Lock(&self.lock_);LightLock_Unlock(&self.lock_);
        LightEvent_Signal(&self.done_);
    }
}
#endif
}
