#pragma once
#include "save.hpp"
#include <3ds.h>
#include <atomic>
namespace hk3ds {
// A worker owns a value snapshot. The latest revision is retried/coalesced by the main thread.
class SaveWriter {
    TutorialSave snapshot_{};
    const char* path_=nullptr; // main's constant path outlives this adapter
    Thread worker_=nullptr;
    std::atomic<bool> done_{false};
    bool running_=false,ok_=false;
    unsigned revision_=0,committed_=0;
    u64 ticks_=0;
    SaveCursor cursor_{}; // this session is the only writer of path_
    SaveOperationTicks operations_{};
    static void run(void* context);
    void finish(bool wait);
public:
    ~SaveWriter(){finish(true);probeSaveStorage(nullptr,nullptr);}
    void update(const char* path,const TutorialSave& snapshot,unsigned revision);
    void flush(const char* path,const TutorialSave& snapshot,unsigned revision);
};
}
