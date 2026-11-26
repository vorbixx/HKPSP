#pragma once
#ifdef __PSP__
#include "draw_list.hpp"
#include "memory_report.hpp"
#include <cstdio>
#include <stdexcept>

namespace hkpsp {
// Shipped counters: IDs reuse reserved storage; timers bracket native I/O and
// validation only. No clock is read on a frame without a load.
struct PageCounters {
    unsigned loads=0,openUs=0,readUs=0,verifyUs=0,evictions=0,residentBytes=0,blockingMiss=0;
    unsigned overflowFrames=0,geWaits=0,geWaitUs=0;
    unsigned vramBytes=0,vramFreeBytes=0,poolSlots=0,fixedPages=0,passEvictions=0;
    std::vector<unsigned> loadedIds;
    void writeCsv(FILE* file,unsigned frame)const;
};
extern PageCounters pageFrame,pageTotals;
void writePageDone(FILE* file);
struct Free {bool pooled=false;void operator()(unsigned char* p)const{if(!pooled)memory_report::releaseAligned(p);}};
using Buffer=std::unique_ptr<unsigned char,Free>;
inline Buffer aligned(unsigned size,memory_report::Category category=memory_report::Category::Other){
    Buffer b(static_cast<unsigned char*>(memory_report::allocateAligned(size,category)));
    return b;
}
class PageFile;
struct Page {
    Buffer indices,palettes;
    // HPT1 dimensions are u16; keeping its bounded metadata compact leaves
    // room for pass history without growing each page's heap allocation.
    unsigned short width=0,height=0;
    unsigned char bits=8;
    bool required=false,directVram=false;
    unsigned lastUse=0,loads=0,evictions=0,reportedEvictions=0;
    // Saturated ordering hints: beyond 65535 passes eviction falls back to LRU.
    unsigned short lastPass=0,previousPass=0;
    unsigned bytes()const{return width*height*bits/8+(bits==4?64:3072);}
    void evict(){indices.reset();palettes.reset();++evictions;}
    explicit Page(const Texture& t):width(t.width),height(t.height),bits(t.bits){
        if(t.width>65535||t.height>65535||(t.bits!=4&&t.bits!=8))
            throw std::runtime_error("Invalid page dimensions or format");
    }
#ifdef HKPSP_GPU_ATTRIB
    const void* vram=nullptr;
    bool visited=false;
    const void* texels()const{return vram?vram:indices.get();}
#endif
    Page(const Texture& t,const char* path);
    void load(const Texture& t,PageFile& file,const char* path);
};
// One owner per backend: fixed storage and one persistent native descriptor.
// Declare before the page table so storage outlives every borrowed page view.
class PageResidency {
    struct State;
    std::unique_ptr<State> state;
    Buffer pool;
    unsigned poolCapacity=0;
public:
    PageResidency();
    ~PageResidency();
    // Drop room bindings after all borrowed Pages die. Keep storage across gameplay rooms.
    void reset(bool retainPool);
    // The backend lends an uncached, 16-byte-aligned region disjoint from all
    // display/composition targets. T4 pages are fixed; one T8 slot follows them.
    void initialize(const std::vector<Texture>& textures,unsigned slotLimit=~0u,
                    void* vram=nullptr,unsigned vramBytes=0);
    unsigned slots()const;
    unsigned poolBytes()const;
    bool fixed(const Page& page)const;
    void beginPass(std::vector<Page>& pages,unsigned frame,bool advance=true);
    void used(Page& page)const;
    bool later(const Page& page)const;
    void describe(PageCounters& counters)const;
    void closeFiles();
    void describePages(std::vector<Page>&,const std::vector<Texture>&);
    void load(Page& page,const Texture& texture,unsigned id,const std::vector<Page>& pages);
};
inline constexpr unsigned pagePoolBudget=21*(512*512+3072);
// Borrowed page lookup for mixed CPU/world streams; ~0u denotes no bank page.
struct PageSequence {
    const void* context;
    unsigned count;
    unsigned (*page)(const void*,unsigned);
};
unsigned preparePages(PageResidency&,std::vector<Page>&,const std::vector<Texture>&,
                      PageSequence,unsigned frame,unsigned target=pagePoolBudget,unsigned firstQuad=0);
// Returns the exclusive end of the fitting quad range. A short range requires
// a GE finish/sync before clearing required flags and preparing the remainder.
unsigned preparePages(PageResidency& residency,std::vector<Page>& pages,const std::vector<Texture>& textures,
                  const std::vector<Quad>& quads,unsigned frame,unsigned target=pagePoolBudget,
                  unsigned firstQuad=0);
}
#endif
