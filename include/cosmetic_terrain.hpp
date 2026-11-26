#pragma once
#include "physics.hpp"
#include <algorithm>
#include <cmath>

namespace hk3ds::cosmetic_terrain {
constexpr float terrainSkin=.025f;
inline float dot(Vec2 a,Vec2 b){return a.x*b.x+a.y*b.y;}
inline float cross(Vec2 a,Vec2 b){return a.x*b.y-a.y*b.x;}
inline Vec2 subtract(Vec2 a,Vec2 b){return {a.x-b.x,a.y-b.y};}
inline Rect shapeBounds(const TerrainShape& s,const World& w) {
    const auto& b=s.bounds;
    if(b.left<=b.right&&b.bottom<=b.top&&(b.left<b.right||b.bottom<b.top))return b;
    // Synthetic test shapes commonly leave the optional cached bounds at {}.
    if(!w.vertices||!s.count)return {};
    Rect result{w.vertices[s.first].x,w.vertices[s.first].y,
                w.vertices[s.first].x,w.vertices[s.first].y};
    for(std::size_t i=1;i<s.count;++i) {
        const Vec2 p=w.vertices[s.first+i];
        result.left=std::min(result.left,p.x);result.right=std::max(result.right,p.x);
        result.bottom=std::min(result.bottom,p.y);result.top=std::max(result.top,p.y);
    }
    return result;
}
inline bool contains(Rect b,Vec2 p) {
    return p.x>=b.left&&p.x<=b.right&&p.y>=b.bottom&&p.y<=b.top;
}
inline bool insidePolygon(Vec2 p,const World& w,const TerrainShape& s) {
    if(s.kind!=0||!w.vertices||s.count<3)return false;
    bool inside=false;
    for(std::size_t i=0,j=s.count-1;i<s.count;j=i++) {
        const Vec2 a=w.vertices[s.first+i],b=w.vertices[s.first+j];
        if(((a.y>p.y)!=(b.y>p.y))&&p.x<(b.x-a.x)*(p.y-a.y)/(b.y-a.y)+a.x)inside=!inside;
    }
    return inside;
}
// Push a cosmetic point out of filled terrain before sweeping. FX can spawn
// inside a collider at the instant the cocoon disappears; ray queries alone
// miss that case when the world's normal start-inside policy is disabled.
inline bool depenetrate(Vec2& p,const World& w) {
    bool moved=false;
    for(unsigned pass=0;pass<4;++pass) {
        float best=1e30f;Vec2 correction{};
        for(std::size_t i=0;i<w.shapeCount;++i) {
            const auto& s=w.shapes[i];
            if(!s.enabled||(s.flags&(1u|TerrainShape::queryOnly))||
               !contains(shapeBounds(s,w),p)||!insidePolygon(p,w,s))continue;
            float area=0;
            for(std::size_t j=0;j<s.count;++j)area+=cross(w.vertices[s.first+j],w.vertices[s.first+(j+1)%s.count]);
            for(std::size_t j=0;j<s.count;++j) {
                const Vec2 a=w.vertices[s.first+j],b=w.vertices[s.first+(j+1)%s.count],e=subtract(b,a);
                const float len2=dot(e,e);if(len2<=1e-10f)continue;
                const float u=std::max(0.0f,std::min(1.0f,dot(subtract(p,a),e)/len2));
                const Vec2 q{a.x+e.x*u,a.y+e.y*u};
                const float dx=p.x-q.x,dy=p.y-q.y,d2=dx*dx+dy*dy;
                if(d2<best) {
                    const float inv=1/std::sqrt(len2),sign=area>=0?1.0f:-1.0f;
                    correction={sign*e.y*inv,sign*-e.x*inv};best=d2;
                }
            }
        }
        for(std::size_t i=0;i<w.count;++i) {
            const Rect b=w.solids[i].bounds;
            if(!(p.x>b.left&&p.x<b.right&&p.y>b.bottom&&p.y<b.top))continue;
            const float edge[4]={p.x-b.left,b.right-p.x,p.y-b.bottom,b.top-p.y};
            const Vec2 n[4]={{-1,0},{1,0},{0,-1},{0,1}};
            for(unsigned k=0;k<4;++k)if(edge[k]*edge[k]<best){best=edge[k]*edge[k];correction=n[k];}
        }
        if(best==1e30f)break;
        p.x+=correction.x*(std::sqrt(best)+terrainSkin);p.y+=correction.y*(std::sqrt(best)+terrainSkin);moved=true;
    }
    return moved;
}
}
