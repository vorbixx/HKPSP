#ifdef __PSP__
#include "../include/memory_report.hpp"
#include "../include/platform.hpp"
#include "../include/draw_list.hpp"
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <pspsysmem.h>
#include <malloc.h>
#include <fcntl.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>

// MEMSIZE the build writes into PARAM.SFO (psp/Makefile passes PSP_LARGE_MEMORY).
#ifndef HKPSP_PARAM_MEMSIZE
#define HKPSP_PARAM_MEMSIZE -1
#endif

namespace hkpsp::memory_report {
namespace {
constexpr const char* forcedSwitch="ms0:/PSP/GAME/HKPSP/data/force-alloc-failure.cfg";
constexpr std::size_t probeStep=64*1024;
constexpr std::size_t probeLimit=64*1024*1024;
constexpr unsigned categories=unsigned(Category::Count),untracked=categories;
const char* const categoryNames[]={"textures","audio","room_banks","draw_list_scratch","room_quad_cache","other"};
struct alignas(16) Header {void* base;std::size_t charged;unsigned category;};
std::array<std::size_t,categories> live{},high{},atPeak{};
std::size_t peakHeap=0,peakArena=0,pendingSize=0,failureSize=0,failureHeap=0;
unsigned failureFree=0;
bool failed=false;
SceUID accountingLock=-1,ownerThread=-1;
Category phase=Category::Other;
char outputDirectory[256]{};
std::vector<std::string> texturePaths;

struct Lock {
    Lock(){if(accountingLock>=0)sceKernelWaitSema(accountingLock,1,nullptr);}
    ~Lock(){if(accountingLock>=0)sceKernelSignalSema(accountingLock,1);}
};
void add(unsigned category,std::size_t bytes){
    live[category]+=bytes;high[category]=std::max(high[category],live[category]);
}
void retag(const void* pointer,Category category){
    if(!pointer||accountingLock<0)return;
    auto* h=static_cast<Header*>(const_cast<void*>(pointer))-1;
    Lock lock;
    if(h->category==untracked)return;
    live[h->category]-=h->charged;h->category=unsigned(category);add(h->category,h->charged);
}
void recordFailure(std::size_t requested){
    failureSize=requested;failureHeap=static_cast<std::size_t>(mallinfo().uordblks);
    failureFree=static_cast<unsigned>(sceKernelMaxFreeMemSize());failed=true;
    // Write before stack unwinding, without allocating on the exhausted heap.
    char path[320],text[256];
    std::snprintf(path,sizeof path,"%s/error.txt",outputDirectory);
    const int length=std::snprintf(text,sizeof text,
        "hkpsp: allocation failed\ncategory=unknown\nrequested_bytes=%u\nheap_used_bytes=%u\nmax_free_block_bytes=%u\n",
        unsigned(failureSize),unsigned(failureHeap),failureFree);
    const int fd=sceIoOpen(path,PSP_O_WRONLY|PSP_O_CREAT|PSP_O_TRUNC,0777);
    if(fd>=0){sceIoWrite(fd,text,length);sceIoClose(fd);}
}
void allocationHandler(){recordFailure(pendingSize);throw std::bad_alloc();}

std::string joinPath(const char* dir,const char* name){return std::string(dir)+"/"+name;}

// Volatile calls keep the optimized ELF's allocations observable. Probe the
// largest block by binary search, then hold all 64 KiB blocks simultaneously.
void* (*volatile probeAllocate)(std::size_t)=std::malloc;
void (*volatile probeRelease)(void*)=std::free;
std::size_t probeHeap(){
    std::size_t low=0,upper=probeLimit/probeStep;
    while(low<upper){
        const auto middle=(low+upper+1)/2;
        void* block=probeAllocate(middle*probeStep);
        if(block){probeRelease(block);low=middle;}else upper=middle-1;
    }
    return low*probeStep;
}
std::size_t probeTotal(){
    void* blocks[probeLimit/probeStep];unsigned count=0;
    while(count<probeLimit/probeStep){
        void* block=probeAllocate(probeStep);if(!block)break;
        blocks[count++]=block;
    }
    const auto total=count*probeStep;
    while(count)probeRelease(blocks[--count]);
    return total;
}

std::size_t heapUsed(){return static_cast<std::size_t>(mallinfo().uordblks);}

class ReportedPlatform final:public Platform {
    Platform& platform;const char* output;bool report;
    FILE* visits=nullptr;
    int arenas=-1;
    unsigned frame=0;
    char scene[64]="Tutorial_01";
    unsigned visit=0;
    mutable std::size_t visitPeak=0,visitArena=0,visitEntry=0,visitExit=0;
    mutable std::array<std::size_t,categories> visitAtPeak{};
    void writeArena(const char* text,unsigned length)const{
        while(length){
            const int written=sceIoWrite(arenas,text,length);
            if(written<=0)throw std::runtime_error("Cannot write arena memory report");
            text+=written;length-=unsigned(written);
        }
    }
    void sampleVisit()const{
        sample();
        Lock lock;const auto info=mallinfo();
        visitExit=static_cast<std::size_t>(info.uordblks);
        if(!visitEntry)visitEntry=visitExit;
        const auto arena=static_cast<std::size_t>(info.arena);
        if(arenas>=0&&arena>visitArena){
            // Native I/O avoids a second libc FILE buffer changing the arena.
            char row[512];int length=std::snprintf(row,sizeof row,"%u,%s,%u,%u,%u,%u,%u",visit,scene,frame,
                unsigned(arena),unsigned(info.uordblks),unsigned(info.fordblks),unsigned(info.ordblks));
            for(const auto bytes:live)length+=std::snprintf(row+length,sizeof row-unsigned(length),",%u",unsigned(bytes));
            row[length++]='\n';writeArena(row,unsigned(length));
        }
        visitArena=std::max(visitArena,arena);
        if(visitExit>=visitPeak){
            visitPeak=visitExit;visitAtPeak=live;
            std::size_t named=0;
            for(unsigned i=0;i<unsigned(Category::Other);++i)named+=visitAtPeak[i];
            visitAtPeak[unsigned(Category::Other)]=visitExit>=named?visitExit-named:0;
        }
    }
    void writeVisit(){
        if(!visits)return;
        std::fprintf(visits,"%u,%s,%u,%u,%u,%u",visit,scene,unsigned(visitPeak),unsigned(visitArena),unsigned(visitEntry),unsigned(visitExit));
        for(const auto bytes:visitAtPeak)std::fprintf(visits,",%u",unsigned(bytes));
        std::fprintf(visits,"\n");std::fflush(visits);
    }
public:
    ReportedPlatform(Platform& p,const char* out,bool enabled):platform(p),output(out),report(enabled){
        texturePaths.clear();phase=Category::Other;
        peakHeap=peakArena=0;atPeak={};high=live;
        if(report){
            visits=std::fopen(joinPath(out,"memory-scenes.csv").c_str(),"w");
            if(!visits)throw std::runtime_error("Cannot create scene memory report");
            std::fprintf(visits,"visit,scene,peak_sampled_heap_bytes,peak_sampled_heap_arena_bytes,entry_heap_bytes,exit_heap_bytes,textures_at_heap_peak_bytes,audio_at_heap_peak_bytes,room_banks_at_heap_peak_bytes,draw_list_scratch_at_heap_peak_bytes,room_quad_cache_at_heap_peak_bytes,other_at_heap_peak_bytes\n");
            char path[320];std::snprintf(path,sizeof path,"%s/memory-arena.csv",out);
            arenas=sceIoOpen(path,PSP_O_WRONLY|PSP_O_CREAT|PSP_O_TRUNC,0777);
            if(arenas<0){std::fclose(visits);throw std::runtime_error("Cannot create arena memory report");}
            constexpr char header[]="visit,scene,frame,arena_bytes,used_bytes,free_bytes,free_blocks,textures_bytes,audio_bytes,room_banks_bytes,draw_list_scratch_bytes,room_quad_cache_bytes,other_bytes\n";
            try{writeArena(header,sizeof header-1);}catch(...){sceIoClose(arenas);std::fclose(visits);throw;}
        }
    }
    ~ReportedPlatform()override{
        phase=Category::Other;
        if(report){writeVisit();if(visits)std::fclose(visits);if(arenas>=0)sceIoClose(arenas);writeReport(output);}
    }
    Platform& raw(){return platform;}
    hk3ds::Input poll()override{return platform.poll();}
    bool menusEnabled()const override{return platform.menusEnabled();}
    UiInput pollUi()override{return platform.pollUi();}
    std::string uiReplay()const override{return platform.uiReplay();}
    std::string path(const std::string& name)const override{
        if(report)sampleVisit();
        phase=Category::Other;
        auto path=platform.path(name);
        const auto dot=name.rfind('.');const auto extension=dot==std::string::npos?"":name.substr(dot);
        if(extension==".hpt"){
            texturePaths.push_back(path);phase=Category::Textures;
        }else if(extension==".pcm")phase=Category::Audio;
        else if(extension==".bin")phase=Category::RoomBanks;
        return path;
    }
    double seconds()const override{return platform.seconds();}
    std::string audioOutput()const override{return platform.audioOutput();}
    bool audioDiagnostics()const override{return platform.audioDiagnostics();}
    const RenderPose* renderPose()const override{return platform.renderPose();}
    bool roomGeEnabled()const override{return platform.roomGeEnabled();}
    std::string sceneRoot()const override{return platform.sceneRoot();}
    std::string sceneEnemyRoot()const override{return platform.sceneEnemyRoot();}
    std::string inputReplay()const override{return platform.inputReplay();}
    std::string savePath()const override{return platform.savePath();}
    void beginSceneLoad(const char* destination)override{
        if(report){
            sampleVisit();writeVisit();++visit;
            std::snprintf(scene,sizeof scene,"%s",destination);
            visitPeak=visitArena=visitEntry=visitExit=0;visitAtPeak={};
            sampleVisit();
        }
        platform.beginSceneLoad(destination);
    }
    void releaseRoomAssets(unsigned retainedPages)override{
        platform.releaseRoomAssets(retainedPages);
        if(retainedPages>texturePaths.size())throw std::runtime_error("Invalid retained texture count");
        texturePaths.resize(retainedPages);
        phase=Category::RoomBanks;
    }
    void beginFrame()override{phase=Category::Other;platform.beginFrame();}
    void preloadRoomPages(const std::vector<Texture>& textures,const char* scene,bool startup)override{
        platform.preloadRoomPages(textures,scene,startup);
        if(report)sampleVisit();
    }
    void releaseTexture(unsigned page)override{platform.releaseTexture(page);}
    void submit(const DrawList& list)override{
        frame=list.info.frame;
        if(report){
            retag(list.quads.data(),Category::DrawScratch);
            if(list.textures){
                retag(list.textures->data(),Category::Textures);
                for(const auto& texture:*list.textures)retag(texture.indices.data(),Category::Textures);
            }
        }
        platform.submit(list);
        if(report)sampleVisit();
    }
    bool presentMonomonMovie()override{return platform.presentMonomonMovie();}
    void endFrame()override{
        platform.endFrame();
        if(report)sampleVisit();
    }
};
}

// All C++ allocation forms share one header. The category belongs to the block,
// so destruction/reallocation after a phase change still debits its true tag.
void* allocate(std::size_t bytes,std::size_t alignment){
    alignment=std::max(alignment,alignof(Header));
    const auto prefix=(sizeof(Header)+alignment-1)&~(alignment-1);
    for(;;){
        if(bytes<=std::numeric_limits<std::size_t>::max()-prefix){
            if(void* base=memalign(alignment,prefix+std::max<std::size_t>(bytes,1))){
                auto* pointer=static_cast<unsigned char*>(base)+prefix;
                auto* h=reinterpret_cast<Header*>(pointer)-1;
                h->base=base;h->charged=malloc_usable_size(base);h->category=untracked;
                if(accountingLock>=0){
                    Lock lock;
                    h->category=unsigned(sceKernelGetThreadId()==ownerThread?phase:Category::Other);
                    add(h->category,h->charged);
                }
                return pointer;
            }
        }
        pendingSize=bytes;
        const auto handler=std::get_new_handler();
        if(!handler)throw std::bad_alloc();
        handler(); // A returning handler may have released memory: retry.
    }
}
void release(void* pointer)noexcept{
    if(!pointer)return;
    auto* h=static_cast<Header*>(pointer)-1;
    if(h->category!=untracked){Lock lock;live[h->category]-=h->charged;}
    std::free(h->base);
}
void tagRoomQuadCache(const void* pointer){retag(pointer,Category::RoomQuadCache);}
void* allocateAligned(std::size_t bytes,Category category){
    const auto previous=phase;phase=category;
    try{void* p=allocate(bytes,16);phase=previous;return p;}
    catch(...){phase=previous;throw;}
}
void releaseAligned(void* pointer)noexcept{release(pointer);}
void install(const char* out,bool){
    std::snprintf(outputDirectory,sizeof outputDirectory,"%s",out);
    // Fatal I/O diagnostics need categories in ordinary play too. This keeps
    // counters only; reports/probes remain opt-in on successful runs.
    ownerThread=sceKernelGetThreadId();
    if(accountingLock<0){
        accountingLock=sceKernelCreateSema("heap-accounting",0,1,1,nullptr);
        if(accountingLock<0)throw std::runtime_error("Cannot create heap accounting lock");
    }
    std::set_new_handler(allocationHandler);
}
std::unique_ptr<Platform> instrument(Platform& platform,const char* out,bool enabled){
    return std::make_unique<ReportedPlatform>(platform,out,enabled);
}
Platform& unwrap(Platform& platform){
    if(auto* reported=dynamic_cast<ReportedPlatform*>(&platform))return reported->raw();
    return platform;
}
const char* texturePath(unsigned page){return texturePaths.at(page).c_str();}
IoFailure::IoFailure(const char* operation,const char* path,int errorNumber,int nativeError) noexcept{
    // Firmware's user table has 64 slots; libcglue's fdman has 1,024. These
    // queries allocate/open nothing. Audio may switch handles during the scan;
    // kernel/driver-internal handles are not enumerable from user mode.
    const int savedErrno=errno;
    unsigned nativeOpen=0,libcOpen=0;
    for(int fd=0;fd<64;++fd)if(sceIoGetDevType(fd)>=0)++nativeOpen;
    for(int fd=0;fd<1024;++fd)if(fcntl(fd,F_GETFL)>=0)++libcOpen;
    errno=savedErrno;
    std::array<std::size_t,categories> snapshot;
    unsigned used,free;
    {
        Lock lock;
        const auto info=mallinfo();used=unsigned(info.uordblks);free=unsigned(info.fordblks);
        snapshot=live;
    }
    std::size_t named=0;
    for(unsigned i=0;i<unsigned(Category::Other);++i)named+=snapshot[i];
    snapshot[unsigned(Category::Other)]=used>=named?used-named:0;
    const unsigned kernelFree=sceKernelTotalFreeMemSize(),kernelLargest=sceKernelMaxFreeMemSize();
    // Fatal path only. Probe malloc directly (never operator new/new_handler),
    // releasing each block immediately; report the 1 KiB lower bound explicitly.
    constexpr unsigned step=1024;
    unsigned low=0,upper=unsigned(probeLimit)/step;
    while(low<upper){
        const unsigned middle=(low+upper+1)/2;
        void* block=probeAllocate(std::size_t(middle)*step);
        if(block){probeRelease(block);low=middle;}else upper=middle-1;
    }
    int length=std::snprintf(text_,sizeof text_,
        "hkpsp: %s failed\npath=%s\nerrno=%d\nsce_io_error=0x%08x\n"
        "sce_io_open_fds=%u\nsce_io_user_fd_limit=64\nlibc_open_fds=%u\nlibc_fd_slots=1024\nfd_snapshot=sampled_user_tables\n"
        "heap_used_bytes=%u\nheap_free_bytes=%u\nheap_largest_allocatable_block_bytes=%u\nheap_probe_step_bytes=%u\n"
        "kernel_free_bytes=%u\nmax_free_block_bytes=%u\ncategory_basis=allocation_phase_with_owner_retagging\n",
        operation,path,errorNumber,unsigned(nativeError),nativeOpen,libcOpen,used,free,low*step,step,kernelFree,kernelLargest);
    for(unsigned i=0;i<categories&&length>=0&&unsigned(length)<sizeof text_;++i)
        length+=std::snprintf(text_+length,sizeof text_-unsigned(length),"%s_live_bytes=%u\n",categoryNames[i],unsigned(snapshot[i]));
}
void sample(){
    Lock lock;const auto info=mallinfo();const auto used=static_cast<std::size_t>(info.uordblks);
    std::size_t named=0;auto snapshot=live;
    for(unsigned i=0;i<unsigned(Category::Other);++i)named+=snapshot[i];
    // Other includes libc allocations, pre-install allocations and malloc headers.
    snapshot[unsigned(Category::Other)]=used>=named?used-named:0;
    for(unsigned i=0;i<categories;++i)high[i]=std::max(high[i],snapshot[i]);
    peakArena=std::max(peakArena,static_cast<std::size_t>(info.arena));
    if(used>=peakHeap){peakHeap=used;atPeak=snapshot;}
}
void writeCategories(std::FILE* f){
    std::fprintf(f,"category_basis=allocation_phase_with_owner_retagging\n"
        "peak_accounted_heap_bytes=%u\npeak_accounted_arena_bytes=%u\n",unsigned(peakHeap),unsigned(peakArena));
    std::size_t sum=0;
    for(unsigned i=0;i<categories;++i){
        std::fprintf(f,"%s_at_heap_peak_bytes=%u\n%s_high_water_bytes=%u\n",categoryNames[i],unsigned(atPeak[i]),categoryNames[i],unsigned(high[i]));
        sum+=atPeak[i];
    }
    std::fprintf(f,"categories_at_heap_peak_sum_bytes=%u\n",unsigned(sum));
}
void writeReport(const char* out){
    char path[320];std::snprintf(path,sizeof path,"%s/memory.txt",out);
    // The backend owns its existing fields. Append one final category snapshot
    // per run when the wrapper closes, before the done marker.
    std::FILE* f=std::fopen(path,"ab");if(!f)return;
    writeCategories(f);std::fclose(f);
}

AllocationFailure::AllocationFailure(std::string category,std::size_t requested)
    :category_(std::move(category)),requested_(requested){recordFailure(requested);}

const char* AllocationFailure::what() const noexcept{return "allocation failed";}

void writeBootReport(const char* out){
    const unsigned total=sceKernelTotalFreeMemSize();
    const unsigned maxFree=sceKernelMaxFreeMemSize();
    const std::size_t usedBefore=heapUsed();
    const unsigned granted=static_cast<unsigned>(probeHeap());
    const unsigned totalAllocatable=static_cast<unsigned>(probeTotal());
    const unsigned devkit=static_cast<unsigned>(sceKernelDevkitVersion());
    std::FILE* f=std::fopen(joinPath(out,"boot-memory.txt").c_str(),"wb");
    // Diagnostics must never stop the game: a failed write is ignored.
    if(!f)return;
    std::fprintf(f,"total_free_bytes=%u\nmax_free_block_bytes=%u\nheap_used_before_probe_bytes=%u\n"
        "heap_granted_bytes=%u\nheap_probe_step_bytes=%u\nheap_probe_limit_bytes=%u\nheap_allocatable_64k_bytes=%u\n"
        "model=unavailable (sceKernelGetModel is only in libpspkernel, kernel-mode, not linked)\n"
        "devkit_version=0x%08x\nparam_memsize=%d\n",
        total,maxFree,unsigned(usedBefore),granted,unsigned(probeStep),unsigned(probeLimit),totalAllocatable,devkit,int(HKPSP_PARAM_MEMSIZE));
    sample();writeCategories(f);
    std::fclose(f);
}

bool forcedFailureRequested(){
    SceIoStat stat{};
    return sceIoGetstat(forcedSwitch,&stat)>=0;
}

std::string failureText(const std::exception& error){
    std::string text="hkpsp: "+std::string(error.what())+"\n";
    if(const auto* failure=dynamic_cast<const AllocationFailure*>(&error)){
        text+="category="+failure->category()+"\nrequested_bytes="+std::to_string(failure->requested())+"\n";
    }else if(dynamic_cast<const std::bad_alloc*>(&error)){
        text+="category=unknown\nrequested_bytes="+(failed?std::to_string(failureSize):"unknown")+"\n";
    }
    if(dynamic_cast<const std::bad_alloc*>(&error)){
        text+="heap_used_bytes="+std::to_string(failed?failureHeap:heapUsed())+"\n";
        text+="max_free_block_bytes="+std::to_string(failed?failureFree:static_cast<unsigned>(sceKernelMaxFreeMemSize()))+"\n";
        text+="heap_used_after_unwind_bytes="+std::to_string(heapUsed())+"\n";
    }
    return text;
}
}
void* operator new(std::size_t n){return hkpsp::memory_report::allocate(n,16);}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{hkpsp::memory_report::release(p);}
void operator delete[](void* p)noexcept{::operator delete(p);}
void operator delete(void* p,std::size_t)noexcept{::operator delete(p);}
void operator delete[](void* p,std::size_t)noexcept{::operator delete(p);}
void* operator new(std::size_t n,std::align_val_t a){return hkpsp::memory_report::allocate(n,std::size_t(a));}
void* operator new[](std::size_t n,std::align_val_t a){return ::operator new(n,a);}
void operator delete(void* p,std::align_val_t)noexcept{::operator delete(p);}
void operator delete[](void* p,std::align_val_t)noexcept{::operator delete(p);}
void operator delete(void* p,std::size_t,std::align_val_t)noexcept{::operator delete(p);}
void operator delete[](void* p,std::size_t,std::align_val_t)noexcept{::operator delete(p);}
#endif
