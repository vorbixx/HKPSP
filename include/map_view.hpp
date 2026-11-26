#pragma once
#include "map_data.hpp"
#include "simple_map.hpp"
#include "inventory.hpp"
#include <cstring>

namespace hk3ds {
inline bool bottomMapVisible(bool artworkReady,bool hasRooms,bool menuOpen,const InventoryScreen* inventory=nullptr)
{
    // Top-screen credits do not own the lower LCD. The Map inventory page
    // also uses the lower LCD; only the other inventory pages cover it.
    return artworkReady&&hasRooms&&!menuOpen&&
        (!inventory||!inventory->opened()||inventory->tab()==InventoryTab::Map);
}
inline const mapdata::Room* mapArtwork(const char* id)
{
    if(id)for(const auto& room:mapdata::rooms)if(std::strcmp(room.id,id)==0)return &room;
    return nullptr;
}
inline std::size_t mapRoomIndex(const char* id)
{
    if(id)for(std::size_t i=0;i<mapdata::roomCount;++i)
        if(std::strcmp(mapdata::rooms[i].id,id)==0)return i;
    return mapdata::roomCount;
}
inline bool mapRevealed(const SimpleMap& map,const char* id)
{
    const auto index=mapRoomIndex(id);
    if(index<mapdata::roomCount){
        const auto& state=map.cartographyState();
        const auto& art=mapdata::rooms[index];
        return art.area==1&&state.ownsCrossroads()&&
            (state.mappedRoom(index)||art.rough!=~0u);
    }
    return false;
}
inline unsigned mapInkImage(const CartographySave& state,const mapdata::Room& art,std::size_t index)
{
    return !state.ownsCrossroads()||art.area!=1||
           state.mappedRoom(index)||art.rough==~0u?art.image:art.rough;
}
inline Vec2 mapHeroPosition(const mapdata::Room& art,Rect bounds,Vec2 hero)
{
    const auto fraction=[](float p,float low,float high){
        return std::isfinite(p)&&high>low?std::clamp((p-low)/(high-low),0.f,1.f):.5f;
    };
    // Approximation: physical room bounds substitute for the original tk2d
    // tilemap dimensions until those dimensions are exported for every scene.
    return {art.x+(fraction(hero.x,bounds.left,bounds.right)-.5f)*art.w,
            art.y+(fraction(hero.y,bounds.bottom,bounds.top)-.5f)*art.h};
}
struct MapCamera {
    Vec2 center{};
    bool initialized=false;
    bool update(Vec2 target){
        if(!initialized){center=target;initialized=true;return true;}
        const auto previous=center;
        const auto ease=[](float value,float goal){return std::abs(goal-value)<.002f?goal:value+(goal-value)*.18f;};
        center={ease(center.x,target.x),ease(center.y,target.y)};
        // Keep easing even when this step is smaller than one LCD pixel.
        return center.x!=previous.x||center.y!=previous.y;
    }
    Vec2 project(Vec2 p) const {
        return {160+(p.x-center.x)*mapdata::pixelsPerUnit,
                132-(p.y-center.y)*mapdata::pixelsPerUnit};
    }
};
}
