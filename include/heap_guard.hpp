#pragma once
// Pure header/canary logic for the HEAP_GUARD diagnostic build. No libctru or
// newlib calls here, so the host tests compile it directly. The 3DS wrappers
// live in src/heap_guard.cpp and are compiled only with HK3DS_HEAP_GUARD=1.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace hk3ds::heapguard {

inline constexpr std::uint32_t liveMagic=0xC0DEB10Cu;      // block allocated by the guard
inline constexpr std::uint32_t freedMagic=0xF4EEDB10u;     // block already released
inline constexpr std::uint32_t bypassMagic=0xB7A55ED0u;    // allocated while a report runs
inline constexpr std::uint32_t untrackedIndex=0xFFFFFFFFu; // live block not in the table
inline constexpr std::size_t headerBytes=16;
inline constexpr std::size_t tailBytes=8;
inline constexpr unsigned char tailByte=0xFD;
inline constexpr unsigned char poisonByte=0xDD;
inline constexpr std::uint32_t maxBlockBytes=0x02000000u;  // 32 MiB; larger sizes are corrupt

// Layout in memory: magic(0) size(4) returnAddress(8) index(12), then payload,
// then tailBytes of tailByte. Fields are written with memcpy in native order.
struct Header {
    std::uint32_t magic;
    std::uint32_t size;
    std::uint32_t returnAddress;
    std::uint32_t index;
};
static_assert(sizeof(Header)==headerBytes,"heap guard header must be 16 bytes");

enum class Fault : std::uint8_t { None, HeaderMagic, HeaderSize, HeaderIndex, TailCanary };
enum class FreeVerdict : std::uint8_t { Forward, ForwardBypass, DoubleFree, Foreign, Corrupt };

struct Finding {
    Fault fault;
    std::uint32_t offset; // byte offset inside the header, or inside the tail
};

inline Header readHeader(const unsigned char* block){
    Header h;
    std::memcpy(&h,block,sizeof h);
    return h;
}

inline void writeBlock(unsigned char* block,std::uint32_t magic,std::uint32_t size,
                       std::uint32_t returnAddress,std::uint32_t index){
    const Header h{magic,size,returnAddress,index};
    std::memcpy(block,&h,sizeof h);
    std::memset(block+headerBytes+size,tailByte,tailBytes);
}

// Index of the first byte in which got and want differ (memory order, little endian).
inline std::uint32_t firstDifferentByte(std::uint32_t got,std::uint32_t want){
    for(std::uint32_t i=0;i<4;++i)
        if(((got>>(8*i))&0xFFu)!=((want>>(8*i))&0xFFu))return i;
    return 0;
}

// Verifies one live block against the slot and size the guard recorded for it
// (expectedIndex, expectedSize). The tail is read only at expectedSize, so a
// corrupted header size can never send the check past the block.
inline Finding verifyBlock(const unsigned char* block,std::uint32_t expectedIndex,std::uint32_t expectedSize){
    const Header h=readHeader(block);
    if(h.magic!=liveMagic)return {Fault::HeaderMagic,firstDifferentByte(h.magic,liveMagic)};
    if(h.size!=expectedSize)return {Fault::HeaderSize,4u+firstDifferentByte(h.size,expectedSize)};
    if(h.index!=expectedIndex)return {Fault::HeaderIndex,12u+firstDifferentByte(h.index,expectedIndex)};
    const unsigned char* tail=block+headerBytes+expectedSize;
    for(std::uint32_t i=0;i<tailBytes;++i)
        if(tail[i]!=tailByte)return {Fault::TailCanary,i};
    return {Fault::None,0};
}

// Decides what free() may do with block. tableHoldsBlock is true when the
// guard's live table points at this block; tableSize is the size recorded there
// (ignored when the block is untracked, which then trusts the header size).
// Only Forward and ForwardBypass reach the real allocator; every other verdict
// is reported and leaked.
inline FreeVerdict classifyFree(const unsigned char* block,bool tableHoldsBlock,std::uint32_t tableSize){
    const Header h=readHeader(block);
    if(h.magic==freedMagic)return FreeVerdict::DoubleFree;
    if(h.magic==bypassMagic)return FreeVerdict::ForwardBypass;
    if(h.magic!=liveMagic)return FreeVerdict::Foreign;
    if(h.index!=untrackedIndex&&!tableHoldsBlock)return FreeVerdict::Corrupt;
    if(h.index!=untrackedIndex&&tableSize>maxBlockBytes)return FreeVerdict::Corrupt;
    const std::uint32_t size=h.index!=untrackedIndex?tableSize:h.size;
    if(size>maxBlockBytes)return FreeVerdict::Corrupt;
    if(verifyBlock(block,h.index,size).fault!=Fault::None)return FreeVerdict::Corrupt;
    return FreeVerdict::Forward;
}

inline const char* faultName(Fault fault){
    switch(fault){
    case Fault::None: return "none";
    case Fault::HeaderMagic: return "magic";
    case Fault::HeaderSize: return "size";
    case Fault::HeaderIndex: return "index";
    case Fault::TailCanary: return "tail";
    }
    return "unknown";
}

inline const char* verdictName(FreeVerdict verdict){
    switch(verdict){
    case FreeVerdict::Forward: return "ok";
    case FreeVerdict::ForwardBypass: return "bypass";
    case FreeVerdict::DoubleFree: return "double_free";
    case FreeVerdict::Foreign: return "foreign_pointer";
    case FreeVerdict::Corrupt: return "corrupt";
    }
    return "unknown";
}

// Hook for the 3DS side: receives one report line. Set by the application.
using Sink=void(*)(const char* line);

#if HK3DS_HEAP_GUARD
void setSink(Sink sink);
// Walks the live table and reports the first corrupt block once per session.
// Returns true when corruption has been seen.
bool check(const char* stage);
#else
inline void setSink(Sink){}
inline bool check(const char*){return false;}
#endif

} // namespace hk3ds::heapguard
