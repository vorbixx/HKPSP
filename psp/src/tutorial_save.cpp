#include "tutorial_save.hpp"
#include <cstdio>
#ifdef __PSP__
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <algorithm>
#endif

namespace hkpsp {
TutorialSaveWriter::~TutorialSaveWriter(){finish(true);}

void TutorialSaveWriter::write(){
    ok_=snapshot_.write(path_.c_str(),cursor_,hk3ds::stdioSaveStorage());
#ifdef __PSP__
    // stdio closes each slot write; commit only after the memory-stick driver
    // has flushed it too. done.txt must never race a pending memory-stick save.
    if(ok_){ok_=sceIoSync("ms0:",0)>=0;if(ok_)++syncs_;}
#endif
    if(ok_)++writes_;
    if(!ok_)cursor_={};
    done_.store(true,std::memory_order_release);
}

int TutorialSaveWriter::run(unsigned,void* argument){
    auto* writer=*static_cast<TutorialSaveWriter**>(argument);
    ++writer->workerWrites_;writer->write();
    return 0;
}

void TutorialSaveWriter::finish(bool wait){
    if(!running_||(!wait&&!done_.load(std::memory_order_acquire)))return;
#ifdef __PSP__
    if(worker_>=0){sceKernelWaitThreadEnd(worker_,nullptr);sceKernelDeleteThread(worker_);worker_=-1;}
#endif
    if(!done_.load(std::memory_order_acquire))return;
    if(ok_){committed_=true;committedRevision_=revision_;}
    else std::fprintf(stderr,"Tutorial save failed: %s\n",path_.c_str());
    running_=false;
}

bool TutorialSaveWriter::needsSnapshot(unsigned revision){
    finish(false);
    return !running_&&(!committed_||revision!=committedRevision_);
}

void TutorialSaveWriter::update(const hk3ds::TutorialSave& snapshot,unsigned revision){
    if(!needsSnapshot(revision))return;
    snapshot_=snapshot;revision_=revision;
    done_.store(false,std::memory_order_relaxed);running_=true;
#ifdef __PSP__
    const int priority=std::min(0x77,sceKernelGetThreadCurrentPriority()+1);
    worker_=sceKernelCreateThread("hkpsp-save",run,priority,16*1024,PSP_THREAD_ATTR_USER,nullptr);
    if(worker_>=0){
        auto* context=this;
        if(sceKernelStartThread(worker_,sizeof(context),&context)>=0)return;
        sceKernelDeleteThread(worker_);worker_=-1;
    }
#endif
    // Match the upstream writer's fallback when a worker cannot be created.
    write();
}

bool TutorialSaveWriter::flush(const hk3ds::TutorialSave& snapshot,unsigned revision){
    finish(true);update(snapshot,revision);finish(true);
    return committed_&&committedRevision_==revision;
}
}
