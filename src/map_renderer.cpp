#include "text_capture.hpp"
#include "map_renderer.hpp"
#include <cstdio>
namespace hk3ds {
bool MapRenderer::load()
{
    release();
    page_=C2D_SpriteSheetLoad("romfs:/map-page-0.t3x");
    if(!page_)return false;
    text_=C2D_TextBufNew(256);
    return true;
}
void MapRenderer::release()
{
    if(text_)C2D_TextBufDelete(text_);
    text_=nullptr;
    if(page_)C2D_SpriteSheetFree(page_);
    page_=nullptr;camera_={};
    x_=y_=panX_=panY_=-1;revision_=~0u;current_=~std::size_t(0);
}
void MapRenderer::image(unsigned index,Vec2 anchor,float ppu,u32 color)
{
    const auto& s=mapdata::images[index];
    const float x=anchor.x+s.left*ppu,y=anchor.y-s.top*ppu;
    const float w=(s.right-s.left)*ppu,h=(s.top-s.bottom)*ppu;
    if(x+w<0||x>320||y+h<0||y>240)return;
    const auto page=C2D_SpriteSheetGetImage(page_,0);
    Tex3DS_SubTexture sub{};sub.width=s.w;sub.height=s.h;
    sub.left=float(s.x)/page.tex->width;sub.right=float(s.x+s.w)/page.tex->width;
    sub.top=1-float(s.y)/page.tex->height;sub.bottom=1-float(s.y+s.h)/page.tex->height;
    C2D_ImageTint tint;C2D_PlainImageTint(&tint,color,1);
    C2D_DrawImageAt({page.tex,&sub},std::round(x),std::round(y),0,&tint,w/s.w,h/s.h);
}
bool MapRenderer::needsDraw(const SimpleMap& map,Vec2 hero,const CollectibleProgress* inventory,bool compass) const
{
    if(!page_||map.rooms().empty())return compass_!=compass||revision_!=map.revision();
    const auto& current=map.rooms()[map.current()];
    const auto* art=mapArtwork(current.id.c_str());
    const auto position=art?mapHeroPosition(*art,current.bounds,hero):camera_.center;
    auto camera=camera_;const bool panning=camera.update(position);
    const auto marker=camera.project(position);
    return panning||compass_!=compass||inventoryRevision_!=(inventory?inventory->count:0)||revision_!=map.revision()||current_!=map.current()||
        x_!=std::round(marker.x)||y_!=std::round(marker.y)||
        panX_!=std::round(camera.center.x*mapdata::pixelsPerUnit)||
        panY_!=std::round(camera.center.y*mapdata::pixelsPerUnit);
}
void MapRenderer::draw(const SimpleMap& map,Vec2 hero,const CollectibleProgress* inventory,bool compass,ui::Canvas canvas)
{
    if(!page_||map.rooms().empty())return;
    const auto& current=map.rooms()[map.current()];
    const auto* art=mapArtwork(current.id.c_str());
    const auto position=art?mapHeroPosition(*art,current.bounds,hero):camera_.center;
    camera_.update(position);
    const auto marker=camera_.project(position);
    const int x=std::round(marker.x),y=std::round(marker.y);
    const int panX=std::round(camera_.center.x*mapdata::pixelsPerUnit),panY=std::round(camera_.center.y*mapdata::pixelsPerUnit);
    compass_=compass;
    inventoryRevision_=inventory?inventory->count:0;
    x_=x;y_=y;panX_=panX;panY_=panY;revision_=map.revision();current_=map.current();
    const auto paper=C2D_Color32(213,207,183,255);
    const auto background=C2D_Color32(15,17,23,255);
    C2D_DrawRectSolid(0,0,0,320,240,background);
    if(!art||art->area!=1||!map.cartographyState().ownsCrossroads()){
        if(canvas.text)ui::unavailableMap(canvas);
        return;
    }
    C3D_DepthTest(false,GPU_ALWAYS,GPU_WRITE_COLOR);
    C2D_SetTintMode(C2D_TintMult);
    // The purchased Crossroads map shows authored rough ink until the room is
    // visited or mapped; saved mapping always selects the full original art.
    const auto& cartography=map.cartographyState();
    for(std::size_t i=0;i<sizeof(mapdata::rooms)/sizeof(mapdata::rooms[0]);++i){
        const auto& room=mapdata::rooms[i];
        if(!mapRevealed(map,room.id))continue;
        image(mapInkImage(cartography,room,i),camera_.project({room.x,room.y}),mapdata::pixelsPerUnit,
              C2D_Color32(room.r,room.g,room.b,room.a));
    }
    // GameMap.PositionCompass: the Knight head requires Wayward Compass.
    if(compass&&art)image(mapdata::marker,{float(x),float(y)},22,C2D_Color32(255,250,230,255));
    // A quiet header keeps the original face readable as the map pans below it.
    C2D_DrawRectSolid(0,0,0,320,35,background);
    const unsigned label=art?(current.id=="Tutorial_01"?2:art->area):3;
    const auto index=mapdata::labels[label];
    image(index,{(320.f-mapdata::images[index].w)*.5f,29},1,paper);
    C2D_DrawLine(35,34,C2D_Color32(104,107,111,180),285,34,C2D_Color32(104,107,111,180),1,0);
    C2D_SetTintMode(C2D_TintSolid);
    if(inventory&&text_){
        char counts[192];std::snprintf(counts,sizeof(counts),"Grubs %lu   Eggs %lu   Journals %lu\nMasks %lu (%lu/4)   Vessels %lu (%lu/3)",
            (unsigned long)inventory->grubs,(unsigned long)inventory->rancidEggs,(unsigned long)inventory->journals,
            (unsigned long)(5+inventory->maskShards/4),(unsigned long)(inventory->maskShards%4),
            (unsigned long)(inventory->vesselFragments/3),(unsigned long)(inventory->vesselFragments%3));
        countsText(counts,canvas);
    }
}
}
