#if defined(__PSP__) && defined(HKPSP_DIAG)
#include "diag.hpp"
#include "diag_codec.hpp"
#include "draw_list.hpp"
#include "page_residency.hpp"
#include "tutorial.hpp"
#include <pspkernel.h>
#include <pspgu.h>
#include <pspge.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <pspctrl.h>
#pragma GCC diagnostic pop
#include <pspdebug.h>
#include <pspiofilemgr.h>
#include <malloc.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>

namespace hkpsp::diag {
namespace {
constexpr const char* root="ms0:/PSP/GAME/HKPSPDIAG/diag";
constexpr unsigned stride=512,readbackBytes=stride*height*4,maxEvents=512;
struct Event {unsigned page;std::uintptr_t slot;bool evicted;};
struct Row {
    FrameInfo info{};
    unsigned workUs=0,copyUs=0,quads=0,draws=0,loads=0,evictions=0,passEvictions=0,waits=0,events=0;
    std::array<Event,maxEvents> event{};
};
struct State {
    Ring ring;
    std::array<Row,history> rows{};
    Row current{};
    unsigned draws=0,events=0,started=0,workUs=0,frame=0,every=1,dumps=0;
    unsigned tapeLeft=0,tapeButtons=0,tapeLx=128,tapeLy=128;
    bool pending=false,chord=false,tapeMode=false,detect=false;
    unsigned anomalies=0;
    // Per-column/row line lengths of the previous displayed frame: art lines persist, the
    // reported stripes live for one frame (2026-11-17 detector run: row 236 is art).
    std::array<unsigned short,width> previousColumns{};
    std::array<unsigned short,height> previousRows{};
    FILE* tape=nullptr;
    unsigned char* scratch;
    std::uint32_t* pixels;
    void* displayOffset=nullptr;
    State(unsigned char* pool,std::size_t bytes,unsigned char* encoded,std::uint32_t* image):ring(pool,bytes),scratch(encoded),pixels(image){}
};
State* state=nullptr;
SceUID allocation=-1;
// 2026-11-17 [M] PSP-3000 under PSPLINK: no 28 MiB block was free in the user partition
// ("Cannot allocate diagnostic partition"), although PPSSPP had one. The ring takes the largest
// block that leaves a margin for later thread stacks and handles, up to its full budget.
std::size_t ringBytes=0,partitionBytes=0;
constexpr std::size_t partitionMargin=2*1024*1024,minimumRingBytes=8*1024*1024;
constexpr std::size_t alignedSize(std::size_t n){return (n+63)&~std::size_t(63);}
constexpr std::size_t allocationBytes=alignedSize(sizeof(State))+poolBytes+alignedSize(encodedBound)+readbackBytes;
static_assert(allocationBytes<28*1024*1024,"Diagnostic partition budget");
void write(FILE* f,const void* p,std::size_t n){
    if(std::fwrite(p,1,n,f)!=n)throw std::runtime_error("Diagnostic write failed");
}
void close(FILE* f){if(std::fclose(f))throw std::runtime_error("Diagnostic close failed");}
FILE* output(const char* folder,const char* name){
    char path[256];std::snprintf(path,sizeof path,"%s/%s",folder,name);
    auto* f=std::fopen(path,"wb");if(!f)throw std::runtime_error("Cannot create diagnostic output");return f;
}
void dump(){
    auto& s=*state;
    char folder[192];
    for(unsigned n=1;;++n){
        std::snprintf(folder,sizeof folder,"%s/%u",root,n);SceIoStat stat{};
        if(sceIoGetstat(folder,&stat)>=0)continue;
        if(sceIoMkdir(folder,0777)<0)throw std::runtime_error("Cannot create diagnostic directory");
        break;
    }
    const auto started=sceKernelGetSystemTimeLow();
    auto* csv=output(folder,"diag.csv");
    std::fprintf(csv,"frame,room,hero_x,hero_y,camera_x,camera_y,control_steps,work_us,copy_us,quads,draw_calls,page_loads,evictions,pass_evictions,ge_waits,encoded_bytes,page_slot_events\n");
    for(unsigned i=0;i<s.ring.size();++i){
        const auto& encoded=s.ring.oldest(i);const auto& r=s.rows[encoded.slot];
        char name[48];std::snprintf(name,sizeof name,"frame-%08u.qoi",r.info.frame);
        auto* image=output(folder,name);const auto first=s.ring.firstSize(encoded);
        write(image,s.ring.first(encoded),first);write(image,s.ring.second(),encoded.size-first);close(image);
        const auto& f=r.info;
        std::fprintf(csv,"%u,Tutorial_01,%.9g,%.9g,%.9g,%.9g,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,",
            f.frame,double(f.heroX),double(f.heroY),double(f.cameraX),double(f.cameraY),f.controlSteps,
            r.workUs,r.copyUs,r.quads,r.draws,r.loads,r.evictions,r.passEvictions,r.waits,unsigned(encoded.size));
        for(unsigned e=0;e<r.events;++e)std::fprintf(csv,"%s%c:%u:%08x",e?";":"",r.event[e].evicted?'E':'L',r.event[e].page,unsigned(r.event[e].slot));
        std::fputc('\n',csv);
    }
    if(std::ferror(csv)){std::fclose(csv);throw std::runtime_error("Diagnostic CSV write failed");}
    close(csv);
    auto* summary=output(folder,"capture.txt");const auto heap=mallinfo();
    std::fprintf(summary,"frames=%u\nring_payload_bytes=%u\nring_capacity_bytes=%u\npartition_bytes=%u\nmetadata_bytes=%u\nencoder_scratch_bytes=%u\nreadback_bytes=%u\nheap_cap_bytes=16777216\nheap_arena_bytes=%u\nheap_used_bytes=%u\nwrite_us=%u\nsample_every=%u\n",
        s.ring.size(),unsigned(s.ring.bytes()),unsigned(ringBytes),unsigned(partitionBytes),unsigned(sizeof(State)),unsigned(alignedSize(encodedBound)),readbackBytes,
        unsigned(heap.arena),unsigned(heap.uordblks),sceKernelGetSystemTimeLow()-started,s.every);
    close(summary);
    auto* done=output(folder,"complete.txt");std::fputs("complete\n",done);close(done);++s.dumps;
    // Readback and disk writes finished before this transient confirmation.
    pspDebugScreenSetOffset(unsigned(reinterpret_cast<std::uintptr_t>(s.displayOffset)));
    pspDebugScreenSetXY(1,1);pspDebugScreenPrintf("Saved %u frames: diag/%s",s.ring.size(),std::strrchr(folder,'/')+1);
    sceKernelDelayThread(700000);
}
}
void initialize(unsigned& frames,unsigned& every,bool& test){
    sceIoMkdir("ms0:/PSP/GAME/HKPSPDIAG",0777);sceIoMkdir(root,0777);
    sceIoMkdir("ms0:/PSP/GAME/HKPSPDIAG/out",0777);
    const std::size_t fixedBytes=alignedSize(sizeof(State))+alignedSize(encodedBound)+readbackBytes;
    const std::size_t largest=sceKernelMaxFreeMemSize();
    ringBytes=largest>fixedBytes+partitionMargin?std::min(poolBytes,(largest-fixedBytes-partitionMargin)&~std::size_t(63)):0;
    if(ringBytes<minimumRingBytes)throw std::runtime_error("Diagnostic ring: largest free block "+
        std::to_string(largest)+" B leaves "+std::to_string(ringBytes)+" B (< 8 MiB)");
    partitionBytes=fixedBytes+ringBytes;
    allocation=sceKernelAllocPartitionMemory(PSP_MEMORY_PARTITION_USER,"HKPSP frame ring",PSP_SMEM_High,partitionBytes,nullptr);
    if(allocation<0)throw std::runtime_error("Cannot allocate diagnostic partition: "+std::to_string(partitionBytes)+" B");
    auto* memory=static_cast<unsigned char*>(sceKernelGetBlockHeadAddr(allocation));
    auto* pool=memory+alignedSize(sizeof(State));auto* scratch=pool+ringBytes;
    state=new(memory) State(pool,ringBytes,scratch,reinterpret_cast<std::uint32_t*>(scratch+alignedSize(encodedBound)));
    auto& s=*state;
    SceIoStat detectStat{};
    s.detect=sceIoGetstat((std::string(root)+"/detect.cfg").c_str(),&detectStat)>=0;
    if(s.detect)sceIoRemove("ms0:/PSP/GAME/HKPSPDIAG/out/anomalies.csv");
    if(auto* config=std::fopen((std::string(root)+"/run.cfg").c_str(),"rb")){
        char extra[2]{};const int fields=std::fscanf(config,"frames=%u every=%u %1s",&frames,&s.every,extra);std::fclose(config);
        if(fields!=2||frames<history||frames>1000000||!s.every||s.every>10000)throw std::runtime_error("Invalid diag/run.cfg");
        s.tape=std::fopen((std::string(root)+"/input.txt").c_str(),"rb");
        if(!s.tape)throw std::runtime_error("Missing diag/input.txt");
        s.tapeMode=true;test=true;every=frames;
    }else{frames=0;test=false;}
    pspDebugScreenInit();s.started=sceKernelGetSystemTimeLow();
}
void start(hk3ds::TutorialSession& game){
    if(auto* config=std::fopen((std::string(root)+"/start.cfg").c_str(),"rb")){
        char room[64]{},facing[16]{},extra[2]{};float x=0,y=0;
        const int fields=std::fscanf(config,"room=%63s hero=%f,%f facing=%15s %1s",room,&x,&y,facing,extra);std::fclose(config);
        if(fields!=4||std::strcmp(room,"Tutorial_01")||!std::isfinite(x)||!std::isfinite(y)||
           (std::strcmp(facing,"right")&&std::strcmp(facing,"left")))throw std::runtime_error("Invalid diag/start.cfg (Tutorial_01 only)");
        hk3ds::InputLatch neutral;neutral.sample(0,0,0);
        for(unsigned tick=0;tick<780;++tick)game.advance(hk3ds::tuning::controlStep,neutral);
        game.flow.phase=hk3ds::RoomPhase::Playing;
        game.simulation.hero.position={x,y};game.simulation.hero.velocity={};
        game.simulation.hero.facingRight=!std::strcmp(facing,"right");
        game.flow.camera={x,y};
    }
    nextWork();
}
void controller(unsigned& buttons,unsigned char& lx,unsigned char& ly){
    auto& s=*state;
    if(s.tapeMode){
        if(!s.tapeLeft){
            if(std::fscanf(s.tape," ticks=%u buttons=%x lx=%u ly=%u",&s.tapeLeft,&s.tapeButtons,&s.tapeLx,&s.tapeLy)!=4||
               !s.tapeLeft||s.tapeLx>255||s.tapeLy>255)throw std::runtime_error("Diagnostic tape ended early or is invalid");
        }
        --s.tapeLeft;buttons=s.tapeButtons;lx=static_cast<unsigned char>(s.tapeLx);ly=static_cast<unsigned char>(s.tapeLy);
    }
    constexpr unsigned chord=PSP_CTRL_SELECT|PSP_CTRL_START;
    const bool held=(buttons&chord)==chord;
    if(held&&!s.chord)s.pending=true;
    s.chord=held;
    if(held)buttons&=~chord;
}
void begin(){state->draws=state->events=0;}
void draw(){++state->draws;}
void page(unsigned id,bool evicted,const void* slot){
    auto& s=*state;if(s.events==maxEvents)throw std::runtime_error("Diagnostic page event capacity exceeded");
    s.current.event[s.events++]={id,reinterpret_cast<std::uintptr_t>(slot),evicted};
}
// 2026-11-17 P1 stripes: the PSP-3000 climb capture held one frame (168) with a 1-px bright
// vertical line that lived for a single frame. With diag/detect.cfg present, every displayed frame
// is scanned for straight 1-px lines (bright or dark against equal neighbours); only those frames
// are saved (out/anomaly-<frame>.qoi + out/anomalies.csv), so long runs collect statistics.
constexpr unsigned maxAnomalies=40,minLineLength=16;
int luma(const std::uint32_t* p,unsigned x,unsigned y){
    const auto c=p[y*stride+x];return int(c&255u)+int((c>>8)&255u)+int((c>>16)&255u);
}
bool oddLine(int a,int b,int c){const int d=2*b-a-c;return (d>90||d< -90)&&std::abs(a-c)<30;}
void scanLines(const FrameInfo& info,const PageCounters& pages,unsigned quads){
    auto& s=*state;if(!s.detect||s.anomalies>=maxAnomalies)return;
    unsigned best=0,where=0;bool vertical=true;
    // Only transient lines count: present now, (almost) absent in the previous frame.
    for(unsigned x=1;x+1<width;++x){
        unsigned n=0;for(unsigned y=0;y<height;++y)n+=oddLine(luma(s.pixels,x-1,y),luma(s.pixels,x,y),luma(s.pixels,x+1,y));
        if(n>best&&s.previousColumns[x]<minLineLength/2){best=n;where=x;vertical=true;}
        s.previousColumns[x]=static_cast<unsigned short>(n);
    }
    for(unsigned y=1;y+1<height;++y){
        unsigned n=0;for(unsigned x=0;x<width;++x)n+=oddLine(luma(s.pixels,x,y-1),luma(s.pixels,x,y),luma(s.pixels,x,y+1));
        if(n>best&&s.previousRows[y]<minLineLength/2){best=n;where=y;vertical=false;}
        s.previousRows[y]=static_cast<unsigned short>(n);
    }
    if(best<minLineLength||info.frame==0)return;
    const auto bytes=encodeRgb(s.pixels,width,height,stride,s.scratch,encodedBound);
    char name[64];std::snprintf(name,sizeof name,"anomaly-%06u.qoi",info.frame);
    auto* image=output("ms0:/PSP/GAME/HKPSPDIAG/out",name);write(image,s.scratch,bytes);close(image);
    auto* csv=std::fopen("ms0:/PSP/GAME/HKPSPDIAG/out/anomalies.csv",s.anomalies?"ab":"wb");
    if(!csv)throw std::runtime_error("Cannot write diagnostic anomalies.csv");
    if(!s.anomalies)std::fputs("frame,axis,index,length,camera_x,camera_y,page_loads,evictions,pass_evictions,ge_waits,quads,draws\n",csv);
    std::fprintf(csv,"%u,%s,%u,%u,%.6f,%.6f,%u,%u,%u,%u,%u,%u\n",info.frame,vertical?"x":"y",where,best,
        double(info.cameraX),double(info.cameraY),pages.loads,pages.evictions,pages.passEvictions,pages.geWaits+1,quads,s.draws);
    close(csv);++s.anomalies;
}
void readback(void* offset){
    auto& s=*state;s.displayOffset=offset;
    sceKernelDcacheWritebackInvalidateRange(s.pixels,readbackBytes);
    auto* source=static_cast<unsigned char*>(sceGeEdramGetAddr())+reinterpret_cast<std::uintptr_t>(offset);
    sceGuCopyImage(GU_PSM_8888,0,0,width,height,stride,source,0,0,stride,s.pixels);
}
void finishWork(){state->workUs=sceKernelGetSystemTimeLow()-state->started;}
void displayed(const FrameInfo& info,const PageCounters& pages,unsigned quads){
    auto& s=*state;s.frame=info.frame;
    if(info.frame%s.every==0){
        const auto started=sceKernelGetSystemTimeLow();
        // GE transfer is complete. CPU must invalidate its previously cached image.
        sceKernelDcacheInvalidateRange(s.pixels,readbackBytes);
        const auto bytes=encodeRgb(s.pixels,width,height,stride,s.scratch,encodedBound);
        const unsigned slot=s.ring.push(s.scratch,bytes);
        auto& r=s.current;r.info=info;r.workUs=s.workUs;r.copyUs=sceKernelGetSystemTimeLow()-started;
        r.quads=quads;r.draws=s.draws;r.loads=pages.loads;r.evictions=pages.evictions;
        r.passEvictions=pages.passEvictions;r.waits=pages.geWaits+1;r.events=s.events;s.rows[slot]=r;
    }
    if(s.detect){
        if(info.frame%s.every)sceKernelDcacheInvalidateRange(s.pixels,readbackBytes);
        scanLines(info,pages,quads);
    }
    if(s.pending){dump();s.pending=false;}
    nextWork();
}
void nextWork(){state->started=sceKernelGetSystemTimeLow();}
void shutdown(){
    if(!state)return;
    if(state->tapeMode&&!state->dumps)dump();
    if(state->tape)close(state->tape);
    state->~State();state=nullptr;sceKernelFreePartitionMemory(allocation);allocation=-1;
}
}
#endif
