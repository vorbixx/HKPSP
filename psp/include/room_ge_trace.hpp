#pragma once
#include "draw_list.hpp"
#include <cstdint>

namespace hkpsp {
// Capture-only evidence at the PSP submission boundary. Geometry is deliberately
// excluded: GE positions are checked by the visual boards and buffer tests.
// This holds no geometry, allocates nothing and accepts each logical draw once.
struct RoomGeTrace {
    std::uint64_t hash=14695981039346656037ull;
    unsigned draws=0,world=0;
    void append(const DrawList&,unsigned first,unsigned last);
};
}
