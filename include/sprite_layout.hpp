#pragma once
#include "animation.hpp"
#include <algorithm>
#include <cmath>

namespace hk3ds
{
struct SpriteLayout
{
    float x, y, scaleX, scaleY;
};
inline SpriteLayout spriteLayout(const SpriteFrame& s, float x, float y, float ppu,
                                 float scaleX, float scaleY)
{
    // citro2d flips texture coordinates for negative scales, but its quad
    // extends right/down from the supplied top-left using absolute dimensions.
    return {x+std::min(s.left*scaleX,s.right*scaleX)*ppu,
            y-std::max(s.top*scaleY,s.bottom*scaleY)*ppu,
            (s.right-s.left)*ppu*scaleX/s.width,
            (s.top-s.bottom)*ppu*scaleY/s.height};
}
struct RotatedSpriteLayout
{
    float x,y,scaleX,scaleY;
    bool flipX,flipY;
};
struct CenteredSpriteQuad
{
    float width,height,centerX,centerY;
};
inline CenteredSpriteQuad centeredSpriteQuad(float width,float height)
{
    // C2D normalizes the rectangle before subtracting its rotation center.
    // Signed dimensions mirror UVs; the geometric center stays positive.
    return {width,height,std::abs(width)*.5f,std::abs(height)*.5f};
}
inline RotatedSpriteLayout rotatedSpriteLayout(const SpriteFrame& s,float x,float y,float ppu,
                                              float scaleX,float scaleY,float rotation)
{
    const float dx=(s.left+s.right)*.5f*scaleX*ppu,dy=(s.bottom+s.top)*.5f*scaleY*ppu;
    const float c=std::cos(rotation),sn=std::sin(rotation);
    // Retain the geometric center separately from the reflection signs.
    // Atlas UVs must remain canonical: top < bottom means packed rotation.
    return {x+dx*c-dy*sn,y-dx*sn-dy*c,
            (s.right-s.left)*ppu*std::abs(scaleX)/s.width,
            (s.top-s.bottom)*ppu*std::abs(scaleY)/s.height,scaleX<0,scaleY<0};
}
}
