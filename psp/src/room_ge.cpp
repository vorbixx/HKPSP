#include "room_ge.hpp"
#include <cmath>

namespace hkpsp {
void RoomGeCounts::add(RoomGeReason reason,const RoomQuadCache* cache){
    ++pairs[static_cast<unsigned>(reason)];
    if(reason!=RoomGeReason::Eligible||!cache)return;
    const auto& q=cache->world;
    // Only unrotated, constant-depth rectangles could use transform-mode sprites.
    if(q.inverseDepth[0]==q.inverseDepth[1]&&q.inverseDepth[0]==q.inverseDepth[2]&&q.inverseDepth[0]==q.inverseDepth[3]&&
       q.positions[0].x==q.positions[3].x&&q.positions[1].x==q.positions[2].x&&
       q.positions[0].y==q.positions[1].y&&q.positions[2].y==q.positions[3].y)++flatPairs;
}
RoomGeReason classifyRoomGe(const RoomQuadCache& cache,const hk3ds::RoomCameraBounds& bounds,float focal){
    if(cache.homogeneousClip)return RoomGeReason::SplitClip;
    // The clamp-before-alpha backend pass copies a screen-space rectangle.
    if(cache.world.blend==Blend::ClampedAdd)return RoomGeReason::Material;
    // A clamped reciprocal loses the original z. Conservatively keep z == 10
    // as well as z < 10 on the CPU; no clamped corner enters a world buffer.
    for(float iz:cache.world.inverseDepth)
        if(!std::isfinite(iz)||iz<=0||iz>=.1f||!std::isfinite(1.f/iz))return RoomGeReason::NearDepth;
    if(!std::isfinite(focal)||focal<=0||!std::isfinite(bounds.left)||!std::isfinite(bounds.right)||
       !std::isfinite(bounds.bottom)||!std::isfinite(bounds.top)||bounds.left>bounds.right||bounds.bottom>bounds.top)
        return RoomGeReason::GuardBand;
    for(unsigned i=0;i<4;++i){
        const auto p=cache.world.positions[i];const float scale=focal*cache.world.inverseDepth[i];
        for(float x:{bounds.left,bounds.right})for(float y:{bounds.bottom,bounds.top}){
            const float sx=screenWidth*.5f+(p.x-x)*scale,sy=screenHeight*.5f-(p.y-y)*scale;
            // One pixel inside the backend's guard band absorbs GE rounding.
            // It is conservative admission, not a relaxed visual tolerance.
            if(!std::isfinite(sx)||!std::isfinite(sy)||sx<-1806||sx>2285||sy<-1910||sy>2181)
                return RoomGeReason::GuardBand;
        }
    }
    return RoomGeReason::Eligible;
}
}
