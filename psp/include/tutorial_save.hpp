#pragma once
#include "save.hpp"
#include <atomic>
#include <string>
#include <utility>

namespace hkpsp {
// One immutable value snapshot per write; revisions coalesce while it is busy.
// Keep this object on the heap, including the snapshot and worker bookkeeping.
class TutorialSaveWriter {
    std::string path_;
    hk3ds::TutorialSave snapshot_{};
    hk3ds::SaveCursor cursor_{};
    std::atomic<bool> done_{false};
    int worker_=-1;
    bool running_=false,ok_=false,committed_=false;
    unsigned revision_=0,committedRevision_=0;
    unsigned writes_=0,workerWrites_=0,syncs_=0;
    void write();
    void finish(bool wait);
    static int run(unsigned,void* argument);
public:
    explicit TutorialSaveWriter(std::string path):path_(std::move(path)){}
    ~TutorialSaveWriter();
    TutorialSaveWriter(const TutorialSaveWriter&)=delete;
    TutorialSaveWriter& operator=(const TutorialSaveWriter&)=delete;
    bool needsSnapshot(unsigned revision);
    void update(const hk3ds::TutorialSave& snapshot,unsigned revision);
    bool flush(const hk3ds::TutorialSave& snapshot,unsigned revision);
    // Read only after flush joins the worker (bounded acceptance evidence).
    unsigned writes()const{return writes_;}
    unsigned workerWrites()const{return workerWrites_;}
    unsigned syncs()const{return syncs_;}
};
}
