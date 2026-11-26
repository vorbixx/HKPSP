#pragma once
#include <array>
#include <algorithm>
#include <cstdint>

namespace hk3ds {
// Largest projected rectangles in ordinary and destination-reading draws.
// Separate buckets keep small but costly grab/composite draws in the sample.
// These are not fragment counts. No heap allocation (32 candidates total).
// Key includes the renderer and pass; snapshot bank addresses alternate each frame.
struct RoomOwnerDraw {
    const void* renderer=nullptr;
    std::uint32_t owner=0,index=0,pixels=0,material=0,pass=0;
    bool destinationRead=false;
    bool sameDraw(const RoomOwnerDraw& other) const {
        return renderer==other.renderer&&owner==other.owner&&index==other.index&&pass==other.pass;
    }
};
struct RoomOwnerSample {
    RoomOwnerDraw draw{};
    std::uint32_t ticks=0;
    bool valid=false;
};
class RoomOwnerProfile {
    static constexpr unsigned capacity=16;
    std::array<std::array<RoomOwnerDraw,capacity>,2> largest_{};
    std::array<unsigned,2> count_{};
    RoomOwnerDraw target_{};
    bool hasTarget_=false;
public:
    RoomOwnerSample sample{};
    void begin(unsigned sampleOrdinal) {
        const unsigned total=count_[0]+count_[1];
        hasTarget_=total!=0;
        if(hasTarget_){
            unsigned at=sampleOrdinal%total;
            const unsigned bucket=at<count_[0]?0:1;
            if(bucket)at-=count_[0];
            target_=largest_[bucket][at];
        }
        count_={};sample={};
    }
    bool consider(const RoomOwnerDraw& draw,bool selected) {
        // Stable ties keep authored draw order. Candidates are refreshed each
        // frame so moving cameras and room changes cannot retain stale targets.
        auto& count=count_[unsigned(draw.destinationRead)];
        auto& largest=largest_[unsigned(draw.destinationRead)];
        unsigned at=0;
        while(at<count&&largest[at].pixels>=draw.pixels)++at;
        if(draw.pixels&&at<capacity){
            const unsigned end=std::min(count,capacity-1);
            for(unsigned i=end;i>at;--i)largest[i]=largest[i-1];
            largest[at]=draw;count=std::min(count+1,capacity);
        }
        if(!selected||sample.valid||!hasTarget_||!target_.sameDraw(draw))return false;
        sample.draw=draw;sample.valid=true;return true;
    }
};
}
