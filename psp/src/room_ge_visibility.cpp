#include "room_ge.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace hkpsp {
namespace {
// Ordered float keys allow bounded bisection through every representable
// camera coordinate, including negative coordinates and both signed zeros.
std::uint32_t key(float value){
    std::uint32_t bits;std::memcpy(&bits,&value,sizeof(bits));
    return bits&0x80000000u?~bits:bits^0x80000000u;
}
float coordinate(std::uint32_t ordered){
    const auto bits=ordered&0x80000000u?ordered^0x80000000u:~ordered;
    float value;std::memcpy(&value,&bits,sizeof(value));return value;
}
template<class Predicate> float first(float low,float high,Predicate accepts){
    auto a=key(low),b=key(high);
    while(a<b){const auto m=a+(b-a)/2;if(accepts(coordinate(m)))b=m;else a=m+1;}
    return coordinate(a);
}
template<class Predicate> float last(float low,float high,Predicate accepts){
    auto a=key(low),b=key(high);
    while(a<b){const auto m=b-(b-a)/2;if(accepts(coordinate(m)))a=m;else b=m-1;}
    return coordinate(a);
}
}
hk3ds::RoomCameraBounds roomGeVisibleCamera(const RoomQuadCache& cache,
    const hk3ds::RoomCameraBounds& source,float focal){
    if(classifyRoomGe(cache,source,focal)!=RoomGeReason::Eligible)
        throw std::invalid_argument("Exact GE visibility requires eligible geometry");
    const hk3ds::RoomCameraBounds empty{1,1,0,0};
    if(!cache.depthVisible||cache.world.rgba[3]<=0)return empty;
    const auto& q=cache.world;
    const auto axis=[&](float camera,bool x,bool minimum){
        float result=0;
        for(unsigned i=0;i<4;++i){
            // Preserve projectRoomQuad's multiplication order. Analytic
            // division followed by nextafter can miss cancellation plateaus.
            const float value=x?screenWidth*.5f+focal*(q.positions[i].x-camera)*q.inverseDepth[i]:
                                screenHeight*.5f-focal*(q.positions[i].y-camera)*q.inverseDepth[i];
            if(i==0)result=value;else result=minimum?std::min(result,value):std::max(result,value);
        }
        return result;
    };
    const auto left=[&](float x){return axis(x,true,true)<=screenWidth;};
    const auto right=[&](float x){return axis(x,true,false)>=0;};
    const auto bottom=[&](float y){return axis(y,false,false)>=0;};
    const auto top=[&](float y){return axis(y,false,true)<=screenHeight;};
    if(!left(source.right)||!right(source.left)||!bottom(source.top)||!top(source.bottom))return empty;
    return {first(source.left,source.right,left),first(source.bottom,source.top,bottom),
            last(source.left,source.right,right),last(source.bottom,source.top,top)};
}
}
