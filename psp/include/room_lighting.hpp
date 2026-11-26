#pragma once
#include "lantern_glow.hpp"
#include "room_visual.hpp"
#include "scene_lighting.hpp"
#include "draw_list.hpp"
#include <utility>

namespace hkpsp {
// Keep the source scale table intact: selectSceneDarkness remaps its first slots.
// Reusing an already remapped table makes later rooms inherit the previous room.
class RoomLighting {
    hk3ds::LightingConfig source;
    hk3ds::SceneLighting current;
public:
    explicit RoomLighting(const hk3ds::LightingConfig& config):source(config){}
    bool load(const char* path,hk3ds::LightingConfig& active,bool lantern=false){
        hk3ds::SceneLighting next;
        if(path&&!next.load(path))return false;
        auto config=source;
        if(path){
            next.configureDarkness(config);
            const auto scales=hk3ds::sceneDarknessScales(source,next.darkness,lantern);
            config.scales[0]=scales[0];config.scales[1]=scales[1];
        }
        current=std::move(next);active=std::move(config);
        return true;
    }
    const hk3ds::SceneLighting& scene()const{return current;}
    void present(DrawList& list,hk3ds::Vec2 hero)const{
        // Match main.cpp's sampleScene: use simulation position, not camera or
        // interpolated presentation, and retain the original lit-owner filter.
        const auto values=current.sample(hero.x,hero.y);
        list.roomAmbient=values.ambientRatio;
        list.cameraColor=values.color;
        list.roomLitOwners=current.hasGrade?&current.litOwners:nullptr;
    }
};

inline void appendLanternGlow(DrawList& list,const hk3ds::LanternGlowConfig& clip,
    const hk3ds::DarknessState& darkness,hk3ds::Vec2 hero,hk3ds::Vec2 camera,unsigned page){
    const float scale=darkness.lanternGlowScale();
    if(scale<=0)return;
    const auto frame=clip.sample(darkness.lanternGlowAge());
    const float ppu=hk3ds::roomFocalLength(screenWidth,screenHeight)/38.106f;
    const float x=screenWidth*.5f+(hero.x-camera.x)*ppu;
    const float y=screenHeight*.5f-(hero.y-camera.y-.5f)*ppu;
    for(unsigned layer=0;layer<4;++layer){
        const auto index=layer*4;
        if(frame[index+3]<=0)continue;
        const auto& sprite=clip.sprites[layer==3?1:0];
        const float halfWidth=sprite.worldWidth*scale*frame[index]*ppu*.5f;
        const float halfHeight=sprite.worldHeight*scale*frame[index+1]*ppu*.5f;
        const float angle=-frame[index+2];
        const float cosine=angle==0?1.f:std::cos(angle),sine=angle==0?0.f:std::sin(angle);
        Quad quad;
        quad.page=page;quad.u0=sprite.x;quad.v0=sprite.y;
        quad.u1=sprite.x+sprite.width;quad.v1=sprite.y+sprite.height;
        quad.rgba[3]=frame[index+3];
        constexpr std::array<Point,4> corners{{{-1,-1},{1,-1},{1,1},{-1,1}}};
        for(unsigned corner=0;corner<corners.size();++corner){
            const float dx=corners[corner].x*halfWidth,dy=corners[corner].y*halfHeight;
            quad.positions[corner]={x+dx*cosine-dy*sine,y+dx*sine+dy*cosine};
        }
        quad.order=list.nextOrder();list.quads.push_back(quad);
    }
}
}
