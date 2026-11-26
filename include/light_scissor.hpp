#pragma once
#include <algorithm>
#include <cmath>
#include <cassert>
#include <array>

namespace hk3ds {
struct LightScissor { unsigned left, bottom, right, top; };
inline bool lightContains(LightScissor outer,LightScissor inner)
{
    return inner.left>=outer.left&&inner.right<=outer.right&&
           inner.bottom>=outer.bottom&&inner.top<=outer.top;
}
// A batch may defer composition only while no source can affect another
// member's padded sampling rectangle. Keep storage bounded on New 3DS.
class LightBatch
{
public:
    static constexpr unsigned capacity=16;
    bool accepts(LightScissor r) const {
        if(count_==capacity)return false;
        for(unsigned i=0;i<count_;++i){
            const auto p=rectangles_[i];
            if(r.left<p.right&&p.left<r.right&&r.bottom<p.top&&p.bottom<r.top)return false;
        }
        return true;
    }
    void append(LightScissor r) {assert(accepts(r));rectangles_[count_++]=r;}
    unsigned size() const {return count_;}
    LightScissor operator[](unsigned i) const {assert(i<count_);return rectangles_[i];}
    void clear() {count_=0;}
private:
    std::array<LightScissor,capacity> rectangles_{};
    unsigned count_=0;
};
// C2D bounds are top-down; the unrotated PICA framebuffer is bottom-up.
// Preserve bilinear support and CPU/float24 projection slack. Invalid bounds
// retain the full pass, including callers which have no projected rectangle.
inline LightScissor lightScissor(float left,float top,float right,float bottom,
                                 unsigned width,unsigned height,
                                 unsigned logicalWidth=0,unsigned logicalHeight=0)
{
    // Scene sprites retain native screen coordinates at a smaller raster size.
    if(logicalWidth&&logicalHeight){
        const float sx=float(width)/logicalWidth,sy=float(height)/logicalHeight;
        left*=sx;right*=sx;top*=sy;bottom*=sy;
    }
    const LightScissor full{0,0,width,height};
    if(!std::isfinite(left)||!std::isfinite(top)||!std::isfinite(right)||!std::isfinite(bottom)||
       left>=right||top>=bottom)return full;
    const auto edge=[](float value,unsigned extent){
        return unsigned(std::clamp(value,0.f,float(extent)));
    };
    LightScissor r{edge(std::floor(left)-3,width),
                   edge(std::floor(float(height)-bottom)-3,height),
                   edge(std::ceil(right)+3,width),
                   edge(std::ceil(float(height)-top)+3,height)};
    return r.left<r.right&&r.bottom<r.top?r:full;
}
struct LightClear { unsigned offset,bytes; };
// RGBA8 RTTs use 8x8 Morton tiles. Physical tile rows run opposite to PICA
// framebuffer Y over the full texture height, rather than the viewport height.
// Clear whole tile rows, padded for bilinear support and independent of X.
inline LightClear lightClear(LightScissor r,unsigned textureWidth,unsigned textureHeight)
{
    assert(textureWidth%8==0&&textureHeight%8==0&&r.left<r.right&&r.bottom<r.top&&
           r.right<=textureWidth&&r.top<=textureHeight);
    const unsigned bottom=textureHeight-r.top,top=textureHeight-r.bottom;
    const unsigned y0=(bottom>2?bottom-2:0)&~7u;
    const unsigned y1=std::min(textureHeight,(top+9u)&~7u);
    const unsigned stride=textureWidth*8u*4u;
    return {y0/8u*stride,stride*(y1-y0)/8u};
}
}
