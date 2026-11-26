#pragma once
#include <cstddef>
#include <vector>

namespace hk3ds {
// Retain headroom for later UI/lighting allocations and allocator fragmentation.
inline constexpr std::size_t roomVramReserveBytes=832u*1024u;
constexpr bool roomVramPageFits(std::size_t freeBytes,std::size_t pageBytes) {
    return freeBytes>=roomVramReserveBytes&&pageBytes<=freeBytes-roomVramReserveBytes;
}
// Stable ties keep the lowest atlas page. Zero-score pages are never promoted.
inline std::size_t hottestRoomPage(const std::vector<double>& scores) {
    std::size_t best=scores.size();double score=0;
    for(std::size_t i=0;i<scores.size();++i)
        if(scores[i]>score){best=i;score=scores[i];}
    return best;
}
}
