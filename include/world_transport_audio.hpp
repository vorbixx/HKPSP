#pragma once
#include "audio.hpp"
#include <algorithm>
#include <cmath>

namespace hk3ds {
// Town FSM3919 uses resources.assets4126 Audio Player Actor / AudioSource13920:
// volume/pitch1, spatial blend1, max50; unweighted custom rolloff keys below.
inline AudioStereoMix townLeverAudioMix(float dx,float dy,float dz)
{
    if(!std::isfinite(dx)||!std::isfinite(dy)||!std::isfinite(dz))return {0,0};
    const float distance=std::sqrt(dx*dx+dy*dy+dz*dz);
    constexpr float span=.9411765f-.78f;
    const float u=std::clamp((distance/50.f-.78f)/span,0.f,1.f);
    const float u2=u*u,u3=u2*u;
    const float gain=std::max(0.f,(2*u3-3*u2+1)+(u3-2*u2+u)*span*-4.545454f+
                                  (u3-u2)*span*-9.596051f);
    // Shared equal-power speaker approximation; source curve remains authored.
    const float pan=distance>0?std::clamp(dx/distance,-1.f,1.f):0;
    const float left=gain*std::sqrt((1-pan)*.5f),right=gain*std::sqrt((1+pan)*.5f);
    return {(left+right)*.5f,left+right>0?(right-left)/(left+right):0};
}
}
