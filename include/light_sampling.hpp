#pragma once
#include <array>
#include <cassert>

namespace hk3ds {
struct LightSampleAxis { std::array<float,4> scene,uv; };
// Preserve the interior texel phase; clamp only the strips outside the first
// and last texel centres. Clamping a single quad shifts every interior sample.
inline LightSampleAxis lightSampleAxis(unsigned scene,unsigned active,unsigned storage)
{
    assert(scene&&active>1&&storage>=active);
    const float edge=.5f*scene/active;
    return {{{0,edge,scene-edge,float(scene)}},
            {{.5f/storage,.5f/storage,(active-.5f)/storage,(active-.5f)/storage}}};
}
}
