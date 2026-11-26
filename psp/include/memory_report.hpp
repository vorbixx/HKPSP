#pragma once
#include "../src/memory_report.hpp"
#include <memory>

namespace hkpsp { class Platform; }
namespace hkpsp::memory_report {
// Fixed storage: capturing/reporting an I/O failure must survive an exhausted
// libc heap. Snapshot at the failing syscall, before page/audio owners unwind.
class IoFailure final:public std::exception {
    char text_[1536]{};
public:
    IoFailure(const char* operation,const char* path,int errorNumber,int nativeError) noexcept;
    const char* what()const noexcept override{return text_;}
};
// Allocation attribution follows the asset-loading phase. Known texture and draw
// buffers are retagged by their owner; libc allocations remain in Other.
enum class Category : unsigned { Textures, Audio, RoomBanks, DrawScratch, RoomQuadCache, Other, Count };
#ifdef __PSP__
void tagRoomQuadCache(const void* pointer);
#else
inline void tagRoomQuadCache(const void*){}
#endif
void install(const char* out,bool diagnostic);
std::unique_ptr<Platform> instrument(Platform& platform,const char* out,bool diagnostic);
Platform& unwrap(Platform& platform);
const char* texturePath(unsigned page);
void* allocateAligned(std::size_t bytes,Category category=Category::Textures);
void releaseAligned(void* pointer) noexcept;
void sample();
void writeReport(const char* out);
}
