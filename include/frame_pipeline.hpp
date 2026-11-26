#pragma once
#include "cpu_worker.hpp"
#include <array>
#include <cassert>
#include <memory>

namespace hk3ds {
// The published slot remains immutable until the render consumer finishes.
// Host and fallback execute exactly the same producer/publication protocol.
template<class State> class FramePipeline {
public:
    using Update=void(*)(const void*,State&);
    FramePipeline():states_(new std::array<State,2>()){}
    FramePipeline(const FramePipeline&)=delete;
    FramePipeline& operator=(const FramePipeline&)=delete;
    ~FramePipeline(){stop();}
    bool start(bool requested){return worker_.start(requested);}
    bool asynchronous() const{return worker_.asynchronous();}
    void submit(const void* context,Update update){
        assert(!pending_&&update);
        context_=context;update_=update;pending_=true;
        worker_.submit(run,this);
    }
    template<class Producer> void submit(const Producer& producer){
        submit(&producer,[](const void* context,State& state){(*static_cast<const Producer*>(context))(state);});
    }
    void finish(){
        if(!pending_)return;
        worker_.wait();published_=writing_;writing_^=1;pending_=false;ready_=true;
    }
    const State& published() const{assert(ready_);return (*states_)[published_];}
    template<class Measure> std::size_t storageBytes(Measure measure) const{
        assert(!pending_);
        return measure((*states_)[0])+measure((*states_)[1]);
    }
    // Initialization only, before any producer or consumer can use the slots.
    std::array<State,2>& initialize(){assert(!ready_&&!pending_);return *states_;}
    void stop(){finish();worker_.stop();}
private:
    std::unique_ptr<std::array<State,2>> states_;
    CpuWorker worker_;
    const void* context_=nullptr;
    Update update_=nullptr;
    unsigned published_=0,writing_=0;
    bool pending_=false,ready_=false;
    static void run(void* context){auto& p=*static_cast<FramePipeline*>(context);p.update_(p.context_,(*p.states_)[p.writing_]);}
};
}
