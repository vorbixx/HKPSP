// Kept empty in the PC build, like the PSP backend.
#ifdef __PSP__
#include "page_residency.hpp"
#include "look_grade.hpp"
#include "perf.hpp"
#include "diag.hpp"
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <cerrno>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace hkpsp {
PageCounters pageFrame,pageTotals;
void PageCounters::writeCsv(FILE* file,unsigned frame)const{
    std::fprintf(file,"%u,%u,%u,%u,%u,%u,%u,%u,",frame,loads,openUs,readUs,verifyUs,
                 evictions,residentBytes,blockingMiss);
    for(unsigned n=0;n<loadedIds.size();++n)std::fprintf(file,"%s%u",n?";":"",loadedIds[n]);
    std::fprintf(file,",%u,%u,%u,%u,%u,%u,%u,%u\n",overflowFrames,geWaits,geWaitUs,
                 vramBytes,vramFreeBytes,poolSlots,fixedPages,passEvictions);
    if(std::ferror(file))throw std::runtime_error("Page counter write failed");
}
void writePageDone(FILE* file){
    std::fprintf(file,"page_loads=%u\npage_open_us=%u\npage_read_us=%u\npage_verify_us=%u\n"
        "page_evictions=%u\npage_resident_bytes=%u\npage_blocking_miss=%u\npage_loaded_ids=",
        pageTotals.loads,pageTotals.openUs,pageTotals.readUs,pageTotals.verifyUs,
        pageTotals.evictions,pageTotals.residentBytes,pageTotals.blockingMiss);
    for(unsigned i=0;i<pageTotals.loadedIds.size();++i)
        std::fprintf(file,"%s%u",i?";":"",pageTotals.loadedIds[i]);
    std::fprintf(file,"\npage_overflow_frames=%u\npage_ge_waits=%u\npage_ge_wait_us=%u\n",
                 pageTotals.overflowFrames,pageTotals.geWaits,pageTotals.geWaitUs);
    std::fprintf(file,"page_vram_bytes=%u\npage_vram_free_bytes=%u\npage_pool_slots=%u\n"
        "page_fixed_pages=%u\npage_pass_evictions=%u\n",pageTotals.vramBytes,pageTotals.vramFreeBytes,
        pageTotals.poolSlots,pageTotals.fixedPages,pageTotals.passEvictions);
}
constexpr unsigned char lightDelta(unsigned p){return p>128?2*p-256:0;}
// 2026-11-16 run 6 [M]: one sceIoOpen per page load cost 13.5 ms on the PSP-3000
// (2,563 loads: open 34.6 s, read 52.9 s). textures.pak (tools/psp/texture_pack.py)
// holds every HPT at a 2048-byte aligned offset; it is opened once and each page
// is a bounded view into it. Without the pack, pages open their own files.
struct TexturePack {
    struct Entry {char name[48];unsigned offset,size;};
    int fd=-1;bool tried=false;std::vector<Entry> entries;
    // The pack covers the pages of ONE directory (texture_pack.py packs data/*.hpt, not subfolders).
    // 2026-11-17 PSP-3000 route test [M]: rooms/Town/static-room-page-9.hpt resolved by basename to
    // the Tutorial pack's static-room-page-9.hpt -> "Invalid texture upload". Other directories
    // always open their own loose files.
    std::string directory;
    const Entry* find(const char* path){
        const char* slash=std::strrchr(path,'/');
        const std::string dir=slash?std::string(path,slash-path):std::string(".");
        if(!tried){
            tried=true;directory=dir;
            const auto pack=dir+"/textures.pak";
            const int file=sceIoOpen(pack.c_str(),PSP_O_RDONLY,0);
            if(file>=0){
                unsigned char header[8];unsigned count=0;
                if(sceIoRead(file,header,8)!=8||std::memcmp(header,"HTX1",4))
                    throw std::runtime_error("Invalid texture pack header: "+pack);
                std::memcpy(&count,header+4,4);
                if(!count||count>4096)throw std::runtime_error("Invalid texture pack count: "+pack);
                entries.resize(count);
                const int bytes=int(count*sizeof(Entry));
                if(sceIoRead(file,entries.data(),unsigned(bytes))!=bytes)
                    throw std::runtime_error("Invalid texture pack index: "+pack);
                for(auto& e:entries){
                    e.name[sizeof e.name-1]=0;
                    if(e.offset%2048)throw std::runtime_error("Unaligned texture pack entry: "+pack);
                }
                fd=file;
            }
        }
        if(fd<0||dir!=directory)return nullptr;
        const char* base=slash?slash+1:path;
        for(const auto& e:entries)if(!std::strcmp(e.name,base))return &e;
        throw std::runtime_error(std::string("Texture missing from textures.pak: ")+base);
    }
};
TexturePack texturePack;
// Native descriptors avoid newlib's allocating FILE pool/read buffer and libc
// descriptor table while the audio worker uses stdio on its own handles.
class PageFile {
    const char* path;
    int fd;bool owned;unsigned base=0,size=~0u,position=0;
    static int errorNumber(int result){
        const auto code=unsigned(result);
        // PSPSDK pspkerror.h: kernel/user or device handle limit.
        if(code==0x80020320u)return EMFILE;
        return (code&0xffff0000u)==0x80010000u?int(code&0xffffu):EIO;
    }
    // The pack handle is shared by every view: position it before each read.
    void seek(){
        const int result=sceIoLseek32(fd,int(base+position),PSP_SEEK_SET);
        if(result!=int(base+position))throw memory_report::IoFailure("Texture seek",path,errorNumber(result),result);
    }
public:
    bool matches(const char* name)const{return !std::strcmp(path,name);}
    explicit PageFile(const char* name):path(name),fd(-1),owned(false){
        const unsigned start=sceKernelGetSystemTimeLow();
        if(const auto* entry=texturePack.find(name)){fd=texturePack.fd;base=entry->offset;size=entry->size;}
        else{fd=sceIoOpen(name,PSP_O_RDONLY,0);owned=true;}
        pageFrame.openUs+=sceKernelGetSystemTimeLow()-start;
        if(fd<0)throw memory_report::IoFailure("Texture open",path,errorNumber(fd),fd);
        if(!owned)seek();
    }
    ~PageFile(){if(owned&&fd>=0)sceIoClose(fd);}
    PageFile(const PageFile&)=delete;
    PageFile& operator=(const PageFile&)=delete;
    void rewind(){
        const unsigned start=sceKernelGetSystemTimeLow();
        position=0;
        if(owned){
            const int result=sceIoLseek32(fd,0,PSP_SEEK_SET);
            if(result<0)throw memory_report::IoFailure("Texture seek",path,errorNumber(result),result);
        }else seek();
        pageFrame.readUs+=sceKernelGetSystemTimeLow()-start;
    }
    bool read(void* buffer,unsigned bytes){
        const unsigned start=sceKernelGetSystemTimeLow();
        auto* next=static_cast<unsigned char*>(buffer);
        if(!owned)seek();
        // A view ends at its entry: reading past it behaves like end of file.
        const bool truncated=bytes>size-position;
        if(truncated)bytes=size-position;
        // 2026-11-16 run 6 [M]: with sceIoRead straight into eDRAM the PSP-3000 froze
        // (USB lost too) at the first combat load into the VRAM page slot; staged
        // through this cached RAM buffer the same tape passes it (4 runs, 600-frame
        // combat complete). Never let sceIoRead target eDRAM.
        const auto physical=reinterpret_cast<std::uintptr_t>(buffer)&0x1FFFFFFFu;
        const bool vram=physical>=0x04000000u&&physical<0x04200000u;
        alignas(64) static unsigned char staging[16384];
        while(bytes){
            // One read per page: 32 KB pieces measured slower on the PSP-3000 (run 7
            // combat: page reads 4.83 -> 7.97 s, work p99 122 -> 168 ms) and did not
            // change audio tail underruns (196 both).
            const int n=vram?sceIoRead(fd,staging,std::min(bytes,unsigned(sizeof staging))):sceIoRead(fd,next,bytes);
            if(n<0){
                // 2026-11-17 [M]: 0x80010016 here was a memory-stick region that stopped reading
                // (textures.pak read back short at 6,127,616 bytes); the target buffer did not
                // matter (stack and staged RAM failed alike). Name the target and the request.
                char detail[256];
                std::snprintf(detail,sizeof detail,"%s buffer=0x%08x request=%u offset=%u vram=%d",path,
                              unsigned(reinterpret_cast<std::uintptr_t>(vram?staging:next)),
                              vram?std::min(bytes,unsigned(sizeof staging)):bytes,base+position,int(vram));
                throw memory_report::IoFailure("Texture read",detail,errorNumber(n),n);
            }
            if(!n){pageFrame.readUs+=sceKernelGetSystemTimeLow()-start;return false;}
#ifdef HKPSP_PERF
            perf::current[perf::TextureBytes]+=unsigned(n);
#endif
            if(vram)std::memcpy(next,staging,unsigned(n));
            next+=n;bytes-=unsigned(n);position+=unsigned(n);
        }
        pageFrame.readUs+=sceKernelGetSystemTimeLow()-start;return !truncated;
    }
};
Page::Page(const Texture& t,const char* path):Page(t){
        // Reserve the handle before consuming the page's own heap buffers.
        PageFile file(path);
        indices=aligned(width*height*bits/8,memory_report::Category::Textures);
        palettes=aligned(bits==4?64:3072,memory_report::Category::Textures);
        load(t,file,path);
}
void Page::load(const Texture& t,PageFile& file,const char* path){
        // Startup already validated the linear view (including HeroLight). Its
        // indices have been released. Read the HPT's GE-ready bytes directly;
        // keep the small CPU palette for linearLightPalette.
        const unsigned row=width*bits/8;
        unsigned char header[16];const unsigned entries=bits==4?16:256;
        const auto u16=[&](unsigned n){return unsigned(header[n])|(unsigned(header[n+1])<<8);};
        unsigned char extra;
        const unsigned start=sceKernelGetSystemTimeLow(),readStart=pageFrame.readUs;
        if(!file.read(header,sizeof header)||
            std::memcmp(header,t.cluts?"HPT2":"HPT1",4)||u16(4)!=width||u16(6)!=height||
            header[8]!=(bits==4?4:5)||header[9]!=3||header[10]!=1||header[11]||
            u16(12)!=(t.cluts?t.cluts->size():entries)||u16(14)||
            (!t.cluts&&(!file.read(palettes.get(),entries*4)||
            std::memcmp(palettes.get(),t.palette.data(),entries*4)))||
            !file.read(indices.get(),row*height)||file.read(&extra,1))
            throw std::runtime_error("Invalid texture upload: "+std::string(path));
        if(!t.cluts)for(unsigned i=0;i<(bits==4?16u:256u);++i)for(unsigned c=0;c<4;++c){
            palettes.get()[i*4+c]=t.palette[i][c];
            if(bits==4)continue;
            palettes.get()[1024+i*4+c]=c==3?t.palette[i][c]:
                static_cast<unsigned char>((unsigned(t.palette[i][c])*t.palette[i][3]+127)/255);
            palettes.get()[2048+i*4+c]=c==3?255:lightDelta(t.palette[i][c]);
        }
        if(!directVram){
            sceKernelDcacheWritebackRange(indices.get(),row*height);
            sceKernelDcacheWritebackRange(palettes.get(),bits==4?64:3072);
        }
        pageFrame.verifyUs+=sceKernelGetSystemTimeLow()-start-(pageFrame.readUs-readStart);
        ++loads;
    }
struct PageResidency::State {
    unsigned char* vram=nullptr;
    unsigned slotBytes=0,count=0,heapCount=0,fontBytes=0,fontPages=0,vramBytes=0,vramFree=0;
    unsigned frame=~0u,pass=0;
    std::optional<PageFile> file;
};
PageResidency::PageResidency()=default;
PageResidency::~PageResidency()=default;
void PageResidency::reset(bool retainPool){
    state.reset();
    if(!retainPool){pool.reset();poolCapacity=0;}
}
void PageResidency::initialize(const std::vector<Texture>& textures,unsigned slotLimit,void* vram,unsigned vramBytes){
    if(state)return;
    state=std::make_unique<State>();
    state->vram=static_cast<unsigned char*>(vram);
    unsigned pooled=0;
    for(const auto& t:textures){
        const Page p(t);
        if(fixed(p)){state->fontBytes+=(p.bytes()+15)&~15u;++state->fontPages;}
        else{state->slotBytes=std::max(state->slotBytes,(p.bytes()+15)&~15u);++pooled;}
    }
    if(vram){
        if((reinterpret_cast<std::uintptr_t>(vram)&15)||state->fontBytes>vramBytes||
           state->slotBytes>vramBytes-state->fontBytes)
            throw std::runtime_error("Page storage exceeds spare VRAM");
        state->vramBytes=state->fontBytes+state->slotBytes;
        state->vramFree=vramBytes-state->vramBytes;
    }
    if(state->slotBytes){
        state->count=std::min({pooled,pagePoolBudget/state->slotBytes+unsigned(vram!=nullptr),slotLimit});
        if(!state->count)throw std::runtime_error("Texture exceeds page pool budget");
        state->heapCount=state->count-unsigned(vram!=nullptr);
        const unsigned required=state->heapCount*state->slotBytes;
        if(required>poolCapacity){
            pool.reset();poolCapacity=0;
            pool=aligned(required,memory_report::Category::Textures);
            poolCapacity=required;
        }
    }
}
unsigned PageResidency::slots()const{return state?state->count:0;}
unsigned PageResidency::poolBytes()const{return state?state->count*state->slotBytes:0;}
bool PageResidency::fixed(const Page& page)const{return state&&state->vram&&page.bits==4;}
void PageResidency::beginPass(std::vector<Page>& pages,unsigned frame,bool advance){
    if(state->frame!=frame){
        for(auto& p:pages){p.previousPass=p.lastPass;p.lastPass=0;}
        state->frame=frame;state->pass=0;
    }
    if(advance||!state->pass)++state->pass;
}
void PageResidency::used(Page& page)const{page.lastPass=std::min(state->pass,65535u);}
bool PageResidency::later(const Page& page)const{return page.previousPass>state->pass;}
void PageResidency::describe(PageCounters& counters)const{
    counters.vramBytes=state->vramBytes;counters.vramFreeBytes=state->vramFree;
    counters.poolSlots=state->count;counters.fixedPages=state->fontPages;
}
void PageResidency::closeFiles(){if(state)state->file.reset();}
void PageResidency::describePages(std::vector<Page>& pages,const std::vector<Texture>& textures){
    if(!pages.empty())return;
    pages.reserve(textures.size());
    pageFrame.loadedIds.reserve(textures.size());
    pageTotals.loadedIds.reserve(textures.size());
    for(const auto& texture:textures)pages.emplace_back(texture);
}
void PageResidency::load(Page& page,const Texture& texture,unsigned id,const std::vector<Page>& pages){
    if(!state||(!fixed(page)&&page.bytes()>state->slotBytes))
        throw std::runtime_error("Texture exceeds initialized page slot");
    const char* path=memory_report::texturePath(id);
    if(!state->file||!state->file->matches(path)){
        state->file.reset();state->file.emplace(path);
    }else state->file->rewind();
    for(unsigned slot=0;slot<state->count+unsigned(fixed(page));++slot){
        unsigned char* data;
        if(fixed(page)){
            unsigned offset=0;for(unsigned i=0;i<id;++i)if(fixed(pages[i]))offset+=(pages[i].bytes()+15)&~15u;
            data=state->vram+offset;
        }else{
            data=slot<state->heapCount?pool.get()+slot*state->slotBytes:state->vram+state->fontBytes;
            if(std::any_of(pages.begin(),pages.end(),[&](const Page& p){return p.indices.get()==data;}))continue;
        }
        page.indices=Buffer(data,Free{true});
        page.palettes=Buffer(data+page.width*page.height*page.bits/8,Free{true});
        page.directVram=fixed(page)||(state->vram&&data==state->vram+state->fontBytes);
        page.load(texture,*state->file,path);HKPSP_DIAG_ONLY(diag::page(id,false,data);)return;
    }
    throw std::runtime_error("No free texture page slot");
}
// firstQuad == 0 starts a frame after the preceding GE sync. Later passes
// mark only their new quads, retaining the earlier passes' required pages
// until the next GE sync. Never replace a submitted page in flight.
unsigned preparePages(PageResidency& residency,std::vector<Page>& pages,const std::vector<Texture>& textures,
                  const std::vector<Quad>& quads,unsigned frame,unsigned target,unsigned firstQuad){
    const PageSequence sequence{&quads,static_cast<unsigned>(quads.size()),[](const void* context,unsigned i){
        const auto& q=static_cast<const std::vector<Quad>*>(context)->at(i);
        if(!samplesTexture(q.blend))return ~0u;
        if(q.page==~0u)throw std::out_of_range("Invalid texture page");
        return q.page;
    }};
    return preparePages(residency,pages,textures,sequence,frame,target,firstQuad);
}
unsigned preparePages(PageResidency& residency,std::vector<Page>& pages,const std::vector<Texture>& textures,
                  PageSequence quads,unsigned frame,unsigned target,unsigned firstQuad){
    if(firstQuad>quads.count)throw std::runtime_error("Invalid page preparation range");
    pageFrame.loads=pageFrame.openUs=pageFrame.readUs=pageFrame.verifyUs=0;
    pageFrame.evictions=pageFrame.residentBytes=pageFrame.blockingMiss=0;
    pageFrame.overflowFrames=pageFrame.geWaits=pageFrame.geWaitUs=0;
    pageFrame.passEvictions=0;
    pageFrame.loadedIds.clear();
    residency.describePages(pages,textures);
    if(!firstQuad)for(auto& p:pages)p.required=false;
    residency.initialize(textures);
    residency.beginPass(pages,frame,false);
    if(target==pagePoolBudget)target=residency.poolBytes();
    unsigned required=0;for(const auto& p:pages)if(p.required&&!residency.fixed(p))++required;
    unsigned endQuad=firstQuad;
    for(;endQuad<quads.count;++endQuad){
        const unsigned id=quads.page(quads.context,endQuad);
        if(id==~0u)continue;
        auto& p=pages.at(id);
        if(!p.required){
            if(!residency.fixed(p)){
                if(required==residency.slots())break;
                ++required;
            }
            p.required=true;
        }
        p.lastUse=frame;residency.used(p);
    }
    unsigned wanted=0;
    for(const auto& p:pages)if(!residency.fixed(p)&&(p.indices||p.required))wanted+=p.bytes();
    unsigned occupied=0;for(const auto& p:pages)if(!residency.fixed(p)&&(p.indices||p.required))++occupied;
    // LRU protects the last 90 frames (and older pages) until the measured hard
    // heap/slot cap forces reuse. Required pages are never eviction candidates.
    while(wanted>target||occupied>residency.slots()){
        Page* oldest=nullptr;
        for(auto& p:pages)if(p.indices&&!p.required&&!residency.fixed(p)&&
            (!oldest||(residency.later(*oldest)&&!residency.later(p))||
             (residency.later(*oldest)==residency.later(p)&&p.lastUse<oldest->lastUse)))oldest=&p;
        if(!oldest)break;
#ifdef HKPSP_PERF
        perf::pageEvent(unsigned(oldest-pages.data()),true);
#endif
        if(!residency.later(*oldest))++pageFrame.passEvictions;
        HKPSP_DIAG_ONLY(diag::page(unsigned(oldest-pages.data()),true,oldest->indices.get());)
        wanted-=oldest->bytes();--occupied;oldest->evict();
    }
    for(unsigned i=0;i<pages.size();++i)if(pages[i].required&&!pages[i].indices){
        residency.load(pages[i],textures[i],i,pages);
        pages[i].required=true;pages[i].lastUse=frame;
        ++pageFrame.loads;pageFrame.loadedIds.push_back(i);
#ifdef HKPSP_PERF
        perf::pageEvent(i,false);
#endif
    }
    pageFrame.blockingMiss=pageFrame.loads?1:0;
    for(auto& p:pages){
        if(p.indices)pageFrame.residentBytes+=p.bytes();
        pageFrame.evictions+=p.evictions-p.reportedEvictions;p.reportedEvictions=p.evictions;
    }
    pageTotals.loads+=pageFrame.loads;pageTotals.openUs+=pageFrame.openUs;
    pageTotals.readUs+=pageFrame.readUs;pageTotals.verifyUs+=pageFrame.verifyUs;
    pageTotals.evictions+=pageFrame.evictions;pageTotals.blockingMiss+=pageFrame.blockingMiss;
    pageTotals.residentBytes=std::max(pageTotals.residentBytes,pageFrame.residentBytes);
    residency.describe(pageFrame);residency.describe(pageTotals);
    pageTotals.passEvictions+=pageFrame.passEvictions;
    for(const auto id:pageFrame.loadedIds)
        if(std::find(pageTotals.loadedIds.begin(),pageTotals.loadedIds.end(),id)==pageTotals.loadedIds.end())
            pageTotals.loadedIds.push_back(id);
    return endQuad;
}
}
#endif
