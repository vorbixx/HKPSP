#pragma once

#include <cstddef>
#include <limits>

namespace hk3ds {

// One explicitly sized native RGBA4 T3X page.
struct ActorTexturePage {
    unsigned width=0;
    unsigned height=0;
};

// Tex3DS stores base-2 dimensions (relative to 8) and the GPU format in its
// first four bytes. Actor atlases use RGBA4; admission charges actual dimensions.
constexpr bool actorTexturePageHeader(const unsigned char* header,std::size_t size,
                                      ActorTexturePage& page) {
    if(!header||size!=4||(header[2]&0xc0)||header[3]!=4)return false;
    page={8u<<(header[2]&7),8u<<((header[2]>>3)&7)};
    return true;
}

constexpr bool actorTexturePageBytes(const ActorTexturePage& page,std::size_t& bytesOut) {
    bytesOut=0;
    if(page.width==0||page.height==0)return false;
    const auto max=std::numeric_limits<std::size_t>::max();
    const auto width=static_cast<std::size_t>(page.width);
    const auto height=static_cast<std::size_t>(page.height);
    if(width>max/2)return false;
    const auto rowBytes=width*2;
    if(height>max/rowBytes)return false;
    bytesOut=rowBytes*height;
    return true;
}

constexpr bool actorTextureBytes(const ActorTexturePage* pages,std::size_t pageCount,
                                 std::size_t& bytesOut) {
    bytesOut=0;
    if(!pages||pageCount==0)return false;
    const auto max=std::numeric_limits<std::size_t>::max();
    std::size_t total=0;
    for(std::size_t i=0;i<pageCount;++i){
        std::size_t pageBytes=0;
        if(!actorTexturePageBytes(pages[i],pageBytes)||pageBytes>max-total)return false;
        total+=pageBytes;
    }
    bytesOut=total;
    return true;
}

constexpr bool actorTextureBytesFit(std::size_t bankBytes,std::size_t futureBytes,
                                    std::size_t freeBytes,std::size_t activeBudget,
                                    std::size_t reserveBytes) {
    const auto max=std::numeric_limits<std::size_t>::max();
    if(futureBytes>max-bankBytes)return false;
    const auto combined=bankBytes+futureBytes;
    if(combined>activeBudget||reserveBytes>max-combined)return false;
    return freeBytes>=combined+reserveBytes;
}

constexpr bool actorTexturePlanFits(const ActorTexturePage* pages,std::size_t pageCount,
                                    std::size_t futureBytes,std::size_t freeBytes,
                                    std::size_t activeBudget,std::size_t reserveBytes,
                                    std::size_t* bankBytesOut=nullptr) {
    if(bankBytesOut)*bankBytesOut=0;
    std::size_t total=0;
    if(!actorTextureBytes(pages,pageCount,total))return false;
    if(bankBytesOut)*bankBytesOut=total;
    return actorTextureBytesFit(total,futureBytes,freeBytes,activeBudget,reserveBytes);
}

} // namespace hk3ds
