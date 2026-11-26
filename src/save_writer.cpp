#include "save_writer.hpp"
#include "frame_profile.hpp"
#include "diagnostic_overlay.hpp"
#include <algorithm>
namespace hk3ds {
void SaveWriter::run(void* context){
    auto& writer=*static_cast<SaveWriter*>(context);
    const auto tick=(diagnosticsEnabled?svcGetSystemTick():0);
    writer.ok_=writer.snapshot_.write(writer.path_,writer.cursor_,stdioSaveStorage());
    writer.ticks_=(diagnosticsEnabled?svcGetSystemTick():0)-tick;
    writer.done_.store(true,std::memory_order_release);
}
void SaveWriter::finish(bool wait){
    if(!running_||(!wait&&!done_.load(std::memory_order_acquire)))return;
    if(worker_){threadJoin(worker_,U64_MAX);threadFree(worker_);worker_=nullptr;}
    frameProfile.saved(ticks_);
    if constexpr(diagnosticsEnabled){
        // Per-operation SD cost of the save that just completed (console A/B).
        const auto ms=[](u64 ticks){return double(ticks)*1000.0/SYSCLOCK_ARM11;};
        diagnosticLog("save_ops %s ok=%u open_ms=%.2f write_ms=%.2f close_ms=%.2f total_ms=%.2f\n",
            operations_.replaced?"replace":"inplace",unsigned(ok_),ms(operations_.open),ms(operations_.write),
            ms(operations_.close),ms(ticks_));
    }
    if(ok_)committed_=revision_;
    running_=false;
}
void SaveWriter::update(const char* path,const TutorialSave& snapshot,unsigned revision){
    finish(false);
    if(running_||revision==committed_)return;
    if(path!=path_)cursor_={};
    path_=path;snapshot_=snapshot;revision_=revision;done_.store(false,std::memory_order_relaxed);running_=true;
    if constexpr(diagnosticsEnabled){operations_={};probeSaveStorage([]()->std::uint64_t{return svcGetSystemTick();},&operations_);}
    s32 priority=0x30;svcGetThreadPriority(&priority,CUR_THREAD_HANDLE);
    worker_=threadCreate(run,this,16*1024,std::min(0x3f,int(priority)+1),-2,false);
    if(!worker_)run(this);
}
void SaveWriter::flush(const char* path,const TutorialSave& snapshot,unsigned revision){
    finish(true);update(path,snapshot,revision);finish(true);
}
}
