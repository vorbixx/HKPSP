// Diagnostic heap guard. Compiled into every build but empty unless the Makefile
// is run with HEAP_GUARD=1, which also adds --wrap for malloc/free/realloc/calloc/memalign.
#include "heap_guard.hpp"

#if HK3DS_HEAP_GUARD
#include <3ds.h>
#include <cstdio>

extern "C" {
void* __real_malloc(size_t);
void __real_free(void*);
void* __real_realloc(void*,size_t);
void* __real_calloc(size_t,size_t);
void* __real_memalign(size_t,size_t);
}

namespace hk3ds::heapguard {
namespace {

// 16384 slots at 4 bytes each is 64 KiB of static RAM, plus 24 bytes per allocation.
constexpr std::uint32_t kSlots=16384;
Header* gTable[kSlots];
std::uint32_t gTableSize[kSlots]; // size recorded at allocation, independent of the header copy
std::uint32_t gHint=0;
LightLock gLock; // zero-initialised LightLock is unlocked
volatile bool gInReport=false; // allocations made while a report runs bypass the table
bool gReported=false;
Sink gSink=nullptr;

struct Guard{
    Guard(){LightLock_Lock(&gLock);}
    ~Guard(){LightLock_Unlock(&gLock);}
};

std::uint32_t findSlot(){
    for(std::uint32_t i=0;i<kSlots;++i){
        const std::uint32_t slot=(gHint+i)%kSlots;
        if(!gTable[slot]){gHint=slot+1;return slot;}
    }
    return untrackedIndex;
}

// Caller holds gLock or is inside a report. Never allocates through the guard.
void report(const char* kind,const char* stage,std::uint32_t size,std::uint32_t lr,
            std::uint32_t index,const char* field,std::uint32_t offset){
    if(!gSink||gInReport)return;
    gInReport=true;
    static char line[256];
    std::snprintf(line,sizeof line,
        "heap_guard kind=%s stage=%s block_size=%lu lr=%08lx index=%08lx field=%s offset=%lu",
        kind,stage?stage:"?",static_cast<unsigned long>(size),static_cast<unsigned long>(lr),
        static_cast<unsigned long>(index),field,static_cast<unsigned long>(offset));
    gSink(line);
    gInReport=false;
}

void* allocate(std::size_t bytes,void* returnAddress){
    if(bytes>maxBlockBytes)return nullptr;
    const std::size_t total=headerBytes+bytes+tailBytes;
    const auto lr=static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(returnAddress));
    if(gInReport){
        auto* raw=static_cast<unsigned char*>(__real_malloc(total));
        if(!raw)return nullptr;
        writeBlock(raw,bypassMagic,static_cast<std::uint32_t>(bytes),lr,untrackedIndex);
        return raw+headerBytes;
    }
    Guard guard;
    const std::uint32_t slot=findSlot();
    auto* raw=static_cast<unsigned char*>(__real_malloc(total));
    if(!raw)return nullptr;
    writeBlock(raw,liveMagic,static_cast<std::uint32_t>(bytes),lr,slot);
    if(slot!=untrackedIndex){
        gTable[slot]=reinterpret_cast<Header*>(raw);
        gTableSize[slot]=static_cast<std::uint32_t>(bytes);
    }
    return raw+headerBytes;
}

// Returns the verdict for p and whether the block is a live guard block in the table.
FreeVerdict checkBlock(void* p,std::uint32_t* size){
    unsigned char* block=static_cast<unsigned char*>(p)-headerBytes;
    if(gInReport){
        const Header h=readHeader(block);
        *size=h.size;
        return h.magic==bypassMagic?FreeVerdict::ForwardBypass:FreeVerdict::Foreign;
    }
    Guard guard;
    const Header h=readHeader(block);
    *size=h.size;
    const bool held=h.index!=untrackedIndex&&h.index<kSlots&&
                    gTable[h.index]==reinterpret_cast<Header*>(block);
    const std::uint32_t tableSize=held?gTableSize[h.index]:0;
    const FreeVerdict verdict=classifyFree(block,held,tableSize);
    if(verdict!=FreeVerdict::Forward&&verdict!=FreeVerdict::ForwardBypass){
        const Finding finding=verifyBlock(block,h.index,held?tableSize:h.size);
        report(verdictName(verdict),"free",h.size,h.returnAddress,h.index,
               faultName(finding.fault),finding.offset);
    }
    return verdict;
}

void freeImpl(void* p){
    if(!p)return;
    unsigned char* block=static_cast<unsigned char*>(p)-headerBytes;
    if(gInReport){
        // Nested inside a report: only bypass blocks may be returned; live blocks are leaked.
        if(readHeader(block).magic==bypassMagic)__real_free(block);
        return;
    }
    Guard guard;
    const Header h=readHeader(block);
    const bool held=h.index!=untrackedIndex&&h.index<kSlots&&
                    gTable[h.index]==reinterpret_cast<Header*>(block);
    const std::uint32_t tableSize=held?gTableSize[h.index]:0;
    const FreeVerdict verdict=classifyFree(block,held,tableSize);
    if(verdict==FreeVerdict::ForwardBypass){__real_free(block);return;}
    if(verdict!=FreeVerdict::Forward){
        const Finding finding=verifyBlock(block,h.index,held?tableSize:h.size);
        report(verdictName(verdict),"free",h.size,h.returnAddress,h.index,
               faultName(finding.fault),finding.offset);
        return; // leak rather than corrupt the allocator
    }
    if(held)gTable[h.index]=nullptr;
    std::memset(p,poisonByte,h.size);
    writeBlock(block,freedMagic,h.size,h.returnAddress,untrackedIndex);
    __real_free(block);
}

} // namespace

void setSink(Sink sink){gSink=sink;}

bool check(const char* stage){
    if(gReported)return true;
    Guard guard;
    for(std::uint32_t slot=0;slot<kSlots;++slot){
        Header* header=gTable[slot];
        if(!header)continue;
        const auto* block=reinterpret_cast<const unsigned char*>(header);
        const Finding finding=verifyBlock(block,slot,gTableSize[slot]);
        if(finding.fault==Fault::None)continue;
        gReported=true;
        report("corrupt",stage,gTableSize[slot],header->returnAddress,slot,
               faultName(finding.fault),finding.offset);
        return true;
    }
    return false;
}

} // namespace hk3ds::heapguard

using hk3ds::heapguard::maxBlockBytes;

extern "C" {

void* __wrap_malloc(size_t bytes){
    return hk3ds::heapguard::allocate(bytes,__builtin_return_address(0));
}

void __wrap_free(void* p){
    hk3ds::heapguard::freeImpl(p);
}

void* __wrap_calloc(size_t count,size_t size){
    if(count&&size>maxBlockBytes/count)return nullptr;
    const size_t bytes=count*size;
    void* p=hk3ds::heapguard::allocate(bytes,__builtin_return_address(0));
    if(p)std::memset(p,0,bytes);
    return p;
}

void* __wrap_realloc(void* p,size_t bytes){
    if(!p)return hk3ds::heapguard::allocate(bytes,__builtin_return_address(0));
    if(bytes==0){hk3ds::heapguard::freeImpl(p);return nullptr;}
    std::uint32_t oldSize=0;
    const auto verdict=hk3ds::heapguard::checkBlock(p,&oldSize);
    if(verdict!=hk3ds::heapguard::FreeVerdict::Forward&&verdict!=hk3ds::heapguard::FreeVerdict::ForwardBypass)
        return nullptr; // reported by checkBlock; the original block is left untouched
    void* q=hk3ds::heapguard::allocate(bytes,__builtin_return_address(0));
    if(!q)return nullptr;
    std::memcpy(q,p,oldSize<bytes?oldSize:bytes);
    hk3ds::heapguard::freeImpl(p);
    return q;
}

void* __wrap_memalign(size_t alignment,size_t bytes){
    // Only alignments that malloc already satisfies go through the guard. Nothing in
    // the project calls memalign; a larger alignment is forwarded untracked and its
    // later free is reported as a foreign pointer.
    if(alignment<=8)return hk3ds::heapguard::allocate(bytes,__builtin_return_address(0));
    return __real_memalign(alignment,bytes);
}

} // extern "C"
#endif
