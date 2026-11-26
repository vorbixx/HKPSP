#include "sector_stream.hpp"
#include "frame_profile.hpp"
#include "sector_prefetch.hpp"
#include "runtime_diagnostics.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <malloc.h>

namespace hk3ds {
namespace {
constexpr std::size_t MiB=1024*1024;
constexpr std::size_t compressedLimit=3*MiB,metadataLimit=512*1024;
void verificationFailure(bool enabled,const char* reason,int x,int y){
    if(!runtimeDiagnostics.enabled()&&(!diagnosticsEnabled||!enabled))return;
    if(auto* f=std::fopen("sdmc:/3ds/HK3DS/room-page-verification-errors.txt","a")){
        std::fprintf(f,"%s sector=%d,%d heap_free=%u linear_free=%u\n",reason,x,y,
                     unsigned(mallinfo().fordblks),unsigned(linearSpaceFree()));
        std::fclose(f);
    }
}
bool boundedFile(const char* path,std::size_t limit,std::vector<std::uint8_t>* bytes=nullptr){
    auto* f=std::fopen(path,"rb");if(!f)return false;
    bool ok=std::fseek(f,0,SEEK_END)==0;const long length=ok?std::ftell(f):-1;
    ok=length>0&&std::size_t(length)<=limit&&std::fseek(f,0,SEEK_SET)==0;
    if(ok&&bytes){bytes->resize(length);ok=std::fread(bytes->data(),1,length,f)==std::size_t(length);}
    std::fclose(f);return ok;
}
}
void SectorStream::run(void* context){
    auto& job=*static_cast<Job*>(context);auto& data=*job.data;
    const auto complete=[&]{
        if(job.observed){job.finishTick=(diagnosticsEnabled?svcGetSystemTick():0);job.phase.store(4,std::memory_order_relaxed);}
        job.done.store(true,std::memory_order_release);
    };
    const auto phase=[&](unsigned value){if(job.observed)job.phase.store(value,std::memory_order_relaxed);};
    const auto cancelled=[&]{
        if(!job.cancel.load(std::memory_order_relaxed))return false;
        job.cancelled=true;complete();return true;
    };
    if(cancelled())return;
    char metadata[128];std::snprintf(metadata,sizeof(metadata),"romfs:/room-%d-%d-visual.bin",data.x,data.y);
    phase(1);auto tick=(diagnosticsEnabled?svcGetSystemTick():0);
    job.ok=boundedFile(metadata,metadataLimit)&&data.visuals.load(metadata)&&data.visuals.atlas.pageCount<=6;
    job.metadataTicks=(diagnosticsEnabled?svcGetSystemTick():0)-tick;
    if(cancelled())return;
    if(!job.ok){complete();return;}
    data.pages.reserve(data.visuals.atlas.pageCount);
    for(unsigned page=0;page<data.visuals.atlas.pageCount;++page){
        if(cancelled())return;
        char path[128];std::snprintf(path,sizeof(path),"romfs:/room-%d-%d-page-%u.t3x",data.x,data.y,page);
        std::vector<std::uint8_t> file;
        phase(2);auto tick=(diagnosticsEnabled?svcGetSystemTick():0);const bool read=boundedFile(path,compressedLimit,&file);
        job.readTicks+=(diagnosticsEnabled?svcGetSystemTick():0)-tick;
        if(cancelled())return;
        if(!read){job.ok=false;break;}
        phase(3);tick=(diagnosticsEnabled?svcGetSystemTick():0);data.pages.emplace_back();
        const bool decoded=decodeRoomPage(file,data.pages.back());
        job.decodeTicks+=(diagnosticsEnabled?svcGetSystemTick():0)-tick;
        if constexpr(diagnosticsEnabled)job.heapMax=std::max(job.heapMax,unsigned(mallinfo().uordblks));
        if constexpr(diagnosticsEnabled)job.linearMin=std::min(job.linearMin,unsigned(linearSpaceFree()));
        if(!decoded){job.ok=false;break;}
        ++job.pages;
    }
    if(cancelled())return;
    complete();
}
void SectorStream::finish(bool wait){
    if(!job_||(!wait&&!job_->done.load(std::memory_order_acquire)))return;
    {FrameProfile::Scope timing(frameProfile,FrameProfile::LoadWait);
     if(worker_){threadJoin(worker_,U64_MAX);threadFree(worker_);worker_=nullptr;}}
    frameProfile.event(3,job_->id,job_->data->x,job_->data->y,4,job_->cancelled?2:job_->ok?1:0,job_->finishTick);
    frameProfile.worker(job_->readTicks,job_->decodeTicks,job_->metadataTicks,job_->pages,job_->heapMax,job_->linearMin);
    if(job_->cancelled)++cancelled_;
    else if(job_->ok){
        observedReason_=2;observedX_=job_->data->x;observedY_=job_->data->y;
        if(frameProfile.enabled())frameProfile.event(8,job_->id,observedX_,observedY_,2,unsigned(job_->data->bytes()));
        if(!cache_.insert(std::move(job_->data))){
            verificationFailure(verify_,"cache insert",observedX_,observedY_);++failures_;
        }
    }
    else {failedX_=job_->data->x;failedY_=job_->data->y;
        verificationFailure(verify_,"worker read/decode/metadata",failedX_,failedY_);++failures_;}
    job_.reset();
}
bool SectorStream::start(int x,int y,const SectorTargets* keep){
    if(job_){frameProfile.deferred(0);return false;}
    if(x==failedX_&&y==failedY_){frameProfile.deferred(3);return false;}
    observedX_=x;observedY_=y;observedJob_=0;
    observedReason_=keep?1:0;
    auto next=std::make_unique<Job>();next->data=std::make_unique<SectorData>();
    auto& data=*next->data;data.x=x;data.y=y;
    // Worst-case complete bank plus metadata, compressed input and allocator/stack margin.
    // Reserve before the worker performs any IO or allocation.
    const auto reservation=data.bytes()+12*MiB+2*metadataLimit+compressedLimit+64*1024;
    frameProfile.event(8,0,x,y,observedReason_,unsigned(reservation));
    if(!cache_.reserve(reservation,keep?keep->data():nullptr,keep?keep->size():0)){
        // Defer speculation when the neighborhood plus scratch cannot fit.
        frameProfile.deferred(4);
        if(!keep){verificationFailure(verify_,"demand reserve",x,y);++failures_;}
        return false;
    }
    next->id=++nextJob_;next->observed=frameProfile.enabled();
    job_=std::move(next);observedJob_=job_->id;
    frameProfile.event(1,job_->id,x,y,0,keep?1:0);
    if(!keep)frameProfile.event(2,job_->id,x,y);
    s32 priority=0x30;svcGetThreadPriority(&priority,CUR_THREAD_HANDLE);
    worker_=threadCreate(run,job_.get(),16*1024,std::min(0x3f,int(priority)+1),-2,false);
    if(!worker_){frameProfile.event(7,job_->id,x,y);FrameProfile::Scope timing(frameProfile,FrameProfile::LoadWait);run(job_.get());}
    return true;
}
const RoomVisualBank* SectorStream::update(int x,int y,float cameraX,float cameraY,RoomRenderer& renderer){
    frameStarted_=false;
    if(!initialized_){
        const auto heap=std::size_t(envGetHeapSize()),live=std::size_t(mallinfo().uordblks);
        // Baseline non-cache peak plus an 8 MiB allocation reserve; never consume linear memory.
        cache_.budget(heap>live+8*MiB?std::min(64*MiB,heap-live-8*MiB):0);
        configuredVerification_=false;
        if(auto* flag=diagnosticsEnabled?std::fopen("sdmc:/3ds/HK3DS/verify-room-pages.flag","rb"):nullptr){configuredVerification_=true;std::fclose(flag);}
        if(frameProfile.enabled()){
            cache_.observerContext=this;
            cache_.observer=[](unsigned kind,int x,int y,std::size_t bytes,void* context){
                auto& stream=*static_cast<SectorStream*>(context);
                frameProfile.event(kind+3,stream.observedJob_,kind==3?x:stream.observedX_,kind==3?y:stream.observedY_,stream.observedReason_,unsigned(bytes));
            };
        }
        initialized_=true;previousX_=cameraX;previousY_=cameraY;
    }
    // Observe before harvesting an already completed job; no extra find/LRU touch.
    verify_=configuredVerification_||runtimeDiagnostics.enabled();
    if(frameProfile.enabled()&&(x!=x_||y!=y_))
        frameProfile.event(2,job_?job_->id:0,x,y,job_?job_->phase.load(std::memory_order_relaxed):0);
    observedJob_=job_?job_->id:0;
    finish(false);
    if(x!=x_||y!=y_){
        auto* data=cache_.find(x,y);
        if(data)++hits_;
        else{
            ++misses_;
            if(job_&&(job_->data->x!=x||job_->data->y!=y)){
                // Demand never waits for a whole unrelated bank. A read/decode in
                // progress finishes normally; cancellation is observed between pages.
                const auto tick=frameProfile.tick();
                job_->cancel.store(true,std::memory_order_relaxed);finish(true);
                frameProfile.obsoleteWait(tick);
            }else finish(true);
            data=cache_.find(x,y);
            if(!data&&start(x,y)){finish(true);data=cache_.find(x,y);}
        }
        if(data){
            if(verify_){
                bool valid=true;
                for(unsigned page=0;page<data->pages.size();++page){
                    char path[128];std::snprintf(path,sizeof(path),"romfs:/room-%d-%d-page-%u.t3x",x,y,page);
                    auto sheet=C2D_SpriteSheetLoad(path);
                    if(!sheet){verificationFailure(verify_,"SDK import",x,y);valid=false;break;}
                    const auto* texture=C2D_SpriteSheetGetImage(sheet,0).tex;
                    valid=texture->size==data->pages[page].size()&&
                        std::memcmp(texture->data,data->pages[page].data(),texture->size)==0;
                    C2D_SpriteSheetFree(sheet);
                    if(!valid){verificationFailure(verify_,"SDK byte mismatch",x,y);break;}
                    ++verified_;
                }
                if(!valid){++failures_;return x_>=0?&visuals_:nullptr;}
            }
            // Copy metadata before changing any submitted texture; raw banks may be evicted.
            RoomVisualBank next=data->visuals;
            if(renderer.upload(*data,verify_)){
                visuals_=std::move(next);x_=x;y_=y;frameStarted_=true;cache_.activate(data);
            }else if(verify_){verificationFailure(verify_,"renderer upload/verification",x,y);++failures_;}
        }
    }
    if(job_)frameProfile.deferred(0);
    if(!job_&&x==x_&&y==y_){
        const float dx=cameraX-previousX_,dy=cameraY-previousY_;
        const auto targets=sectorPrefetch(x,y,cameraX,cameraY,dx,dy);
        for(const auto target:targets){
            // Merely considering a candidate must not promote it over used banks in LRU.
            if(target.x<0){frameProfile.deferred(1);continue;}
            if(cache_.contains(target.x,target.y)){frameProfile.deferred(2);continue;}
            if(start(target.x,target.y,&targets)){++prefetches_;break;}
        }
    }
    previousX_=cameraX;previousY_=cameraY;
    frameProfile.cache(cache_.budget(),cache_.used(),hits_,misses_,prefetches_,failures_,cancelled_);
    return x_>=0?&visuals_:nullptr;
}
void SectorStream::stop(){
    finish(true);
    frameProfile.cache(cache_.budget(),cache_.used(),hits_,misses_,prefetches_,failures_,cancelled_);
    if(verify_||runtimeDiagnostics.enabled()){
        if(auto* f=std::fopen("sdmc:/3ds/HK3DS/room-page-verification.txt","w")){
            std::fprintf(f,"verified=%u failures=%u\n",verified_,failures_);std::fclose(f);
        }
        verify_=false;
    }
    cache_.clear();
}
}
