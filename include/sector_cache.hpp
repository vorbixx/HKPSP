#pragma once
#include "room_visual.hpp"
#include <cstdint>
#include <list>
#include <memory>
#include <vector>

namespace hk3ds {
struct SectorTarget;
// Exact reader for our square tex3ds RGBA4 room/decor pages, raw or LZ11.
// Decoding produces GPU-ready bytes in ordinary heap memory; no GPU APIs here.
bool decodeRoomPage(const std::vector<std::uint8_t>& file,std::vector<std::uint8_t>& output,unsigned width=1024);
struct RoomPageRange {std::size_t offset=0,size=0;};
// RGBA4/1024: union of 8x8 tiles covering sprites and their bilinear border.
// Coordinates are PNG top-left; byte ranges refer to tex3ds GPU-ready data.
bool roomPageRanges(const AnimationBank& atlas,unsigned page,std::vector<RoomPageRange>& ranges);
struct SectorData {
    int x=-1,y=-1;
    RoomVisualBank visuals;
    std::vector<std::vector<std::uint8_t>> pages;
    std::size_t bytes() const;
};
class SectorCache {
    std::list<std::unique_ptr<SectorData>> entries_;
    std::size_t budget_=0,used_=0;
    SectorData* active_=nullptr;
public:
    // Optional main-thread observer: budget failure, protected-set failure, eviction.
    using Observer=void(*)(unsigned,int,int,std::size_t,void*);
    Observer observer=nullptr;
    void* observerContext=nullptr;
    explicit SectorCache(std::size_t budget=0):budget_(budget){}
    void budget(std::size_t bytes){budget_=bytes;}
    std::size_t used() const{return used_;}
    std::size_t budget() const{return budget_;}
    SectorData* active() const{return active_;}
    void activate(SectorData* entry){active_=entry;}
    SectorData* find(int x,int y);
    bool contains(int x,int y) const;
    // Reserve includes worker scratch space. Active and optional predicted banks
    // stay resident during speculation. Failure leaves the cache intact.
    bool reserve(std::size_t bytes,const SectorTarget* keep=nullptr,std::size_t count=0);
    SectorData* insert(std::unique_ptr<SectorData> entry);
    void clear(){entries_.clear();active_=nullptr;used_=0;}
};
}
