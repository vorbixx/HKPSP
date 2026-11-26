#pragma once
#include "draw_list.hpp"
#include "room.hpp"
#include <algorithm>

namespace hkpsp {
// main.cpp:1010-1013: separate black overlays, in death then transition order.
inline void appendRoomFades(DrawList& list,const hk3ds::RoomFlow& flow){
    for(const float fade:{flow.deathFade(),flow.transitionFade()}){
        if(!(fade>0))continue;
        Quad q;q.blend=Blend::FadeBlack;
        // Match the shared host's u8 conversion rather than rounding opacity.
        q.rgba={0,0,0,float(unsigned(255*std::clamp(fade,0.f,1.f)))/255};
        q.positions={{{0,0},{screenWidth,0},{screenWidth,screenHeight},{0,screenHeight}}};
        q.order=unsigned(list.quads.size());list.quads.push_back(q);
    }
}
// main.cpp:1017-1020: the opening blanker is a full-screen black quad drawn after the
// world and before the HUD. Its alpha is TutorialSession::credits.blankerAlpha(); the
// 3DS skips it while the Crossroads boss is active.
inline void appendOpeningBlanker(DrawList& list,float alpha,bool crossroadsBossActive){
    if(crossroadsBossActive||!(alpha>0))return;
    Quad q;q.blend=Blend::FadeBlack;
    // Same shared u8 conversion as the room fades.
    q.rgba={0,0,0,float(unsigned(255*std::clamp(alpha,0.f,1.f)))/255};
    q.positions={{{0,0},{screenWidth,0},{screenWidth,screenHeight},{0,screenHeight}}};
    q.order=list.nextOrder();list.quads.push_back(q);
}
}
