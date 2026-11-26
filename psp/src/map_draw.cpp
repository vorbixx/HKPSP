#include "../include/map_draw.hpp"
#include <algorithm>
#include <stdexcept>

namespace hkpsp {
namespace {
constexpr float panMinX=-1.44f-(-.04f),panMaxX=4.55f-(-.04f);
constexpr float panMinY=-8.642f-(-7.58f),panMaxY=-5.58f-(-7.58f);
constexpr float panStep=8.f/30.f,panPixels=15.5f;
struct Widget { int x,y,width,height; };
constexpr Widget titleBoard{0,336,204,60},zoomBoard{208,336,138,36};
constexpr Widget arrowUp{350,336,15,7},arrowDown{369,336,15,7};
constexpr Widget arrowLeft{388,336,7,15},arrowRight{399,336,7,15};
}
void MapOverlay::update(const UiInput& input,bool allowed,bool ownsMap,bool inCrossroads){
    const auto previous=view_;
    if(!allowed||(input.pressed&UiStart))view_=MapView::Closed;
    else if(view_==MapView::Quick){
        if(!ownsMap||!inCrossroads||!input.selectHold)view_=MapView::Closed;
    }else if(pauses()){
        if(input.selectTap)view_=MapView::Closed;
        // Charms: Circle equips in the game loop (menu confirm); Cross returns to Items, not close.
        else if(view_==MapView::Charms){if(input.pressed&UiCross)view_=MapView::Items;}
        else if(input.pressed&UiCross)view_=view_==MapView::InventoryZoom?MapView::Inventory:MapView::Closed;
        else if(view_==MapView::Items&&ownsMap&&(input.pressed&UiLeft))view_=MapView::Inventory;
        else if(view_==MapView::Items&&(input.pressed&UiRight))view_=MapView::Charms;
        else if(showsMap()&&!ownsMap)view_=MapView::Items;
        else if(view_==MapView::Inventory&&(input.pressed&UiRight))view_=MapView::Items;
        else if(view_==MapView::Inventory&&(input.pressed&UiCircle))view_=MapView::InventoryZoom;
    }else if(input.selectTap)view_=MapView::Items;
    else if(input.selectHoldPressed&&ownsMap&&inCrossroads)view_=MapView::Quick;
    if(view_!=MapView::InventoryZoom||previous!=view_)pan_={};
    else{
        const int horizontal=int(bool(input.held&UiLeft))-int(bool(input.held&UiRight));
        const int vertical=int(bool(input.held&UiDown))-int(bool(input.held&UiUp));
        pan_.x=std::clamp(pan_.x+horizontal*panStep,panMinX,panMaxX);
        pan_.y=std::clamp(pan_.y+vertical*panStep,panMinY,panMaxY);
    }
}

Texture MapAtlas::descriptor(){
    Texture texture;texture.width=texture.height=512;texture.transient=true;
    return texture;
}
MapAtlas::MapAtlas(Platform& platform,std::vector<Texture>& textures)
    :path_(platform.path("map-page-0.hpt")),page_(unsigned(textures.size())){
    textures.push_back(descriptor());
}
void MapAtlas::setVisible(Platform& platform,std::vector<Texture>& textures,bool visible){
    if(visible==loaded_)return;
    if(visible){
        auto texture=readTexture(path_);
        if(texture.width!=512||texture.height!=512||texture.bits!=8||texture.cluts)
            throw std::runtime_error("Crossroads map atlas contract failed");
        texture.transient=true;
        textures.at(page_)=std::move(texture);
    }else{
        platform.releaseTexture(page_);
        textures.at(page_)=descriptor();
    }
    loaded_=visible;
}

void appendMap(DrawList& list,hk3ds::ui::Canvas canvas,unsigned page,MapView view,
               const hk3ds::CartographySave& progress,std::size_t current,hk3ds::Vec2 hero,bool compass,
               hk3ds::Vec2 pan){
    using namespace hk3ds;
    using namespace hk3ds::ui;
    if(!progress.ownsCrossroads()||view==MapView::Closed||view==MapView::Items||view==MapView::Charms)return;
    const bool quick=view==MapView::Quick;
    const bool overview=view==MapView::Inventory;
    const bool zoomed=view==MapView::InventoryZoom;
    const float scale=overview?7.3f:20.f;
    const float centerY=overview?146.f:(quick?138.f:147.f);
    const auto shift=zoomed?Vec2{pan.x*panPixels,-pan.y*panPixels}:Vec2{};
    const auto project=[&](Vec2 point){
        return Vec2{240.f+(point.x-.3f)*scale+shift.x,centerY-(point.y-5.25f)*scale+shift.y};
    };
    const auto widget=[&](Widget art,float centerX,float centerY){
        const float left=centerX-art.width*.5f,top=centerY-art.height*.5f;
        const float right=left+art.width,bottom=top+art.height;
        Quad quad;quad.page=page;
        quad.u0=float(art.x);quad.v0=float(art.y);
        quad.u1=float(art.x+art.width);quad.v1=float(art.y+art.height);
        quad.positions={{{left,top},{right,top},{right,bottom},{left,bottom}}};
        list.quads.push_back(quad);
    };
    const auto image=[&](unsigned index,Vec2 center,float density,std::array<float,4> color){
        const auto& art=mapdata::images[index];
        const float left=center.x+art.left*density,top=center.y-art.top*density;
        const float right=center.x+art.right*density,bottom=center.y-art.bottom*density;
        Quad quad;quad.page=page;quad.rgba=color;
        quad.u0=float(art.x);quad.v0=float(art.y);
        quad.u1=float(art.x+art.w);quad.v1=float(art.y+art.h);
        quad.positions={{{left,top},{right,top},{right,bottom},{left,bottom}}};
        list.quads.push_back(quad);
    };
    for(std::size_t index=0;index<mapdata::roomCount;++index){
        const auto& room=mapdata::rooms[index];
        const bool context=!overview&&room.area==0&&(!quick||room.rough!=~0u);
        const bool crossroads=room.area==1&&(room.rough!=~0u||progress.visitedRoom(unsigned(index)));
        if(!context&&!crossroads)continue;
        const auto art=context&&room.rough!=~0u?room.rough:mapInkImage(progress,room,index);
        image(art,project({room.x,room.y}),scale,
              {room.r/255.f,room.g/255.f,room.b/255.f,room.a/255.f});
    }
    if(compass&&current<mapdata::roomCount&&mapdata::rooms[current].area==1&&progress.visitedRoom(unsigned(current)))
        image(mapdata::marker,project(hero),overview?10.f:11.f,{1.f,1.f,1.f,1.f});
    if(zoomed){
        if(pan.y>panMinY)widget(arrowUp,239,52);
        if(pan.y<panMaxY)widget(arrowDown,240,229);
        if(pan.x<panMaxX)widget(arrowLeft,37,130);
        if(pan.x>panMinX)widget(arrowRight,444,130);
    }
    if(canvas.text){
        if(!quick){
            wrappedText(canvas,"Forgotten\nCrossroads",overview?ui::Rect{238,134,90,40}:
                        ui::Rect{254+shift.x,182+shift.y,90,40},Size::Body);
            const char* zoom=overview?"Zoom In  (Circle)":"Zoom Out  (Cross)";
            const float width=canvas.measure(canvas.user,zoom,Size::Body);
            widget(zoomBoard,408,225);
            canvas.text(canvas.user,zoom,408.f-width*.5f,215.f,Size::Body,white);
        }else{
            const char* title="Forgotten Crossroads";
            const Size size=Size::Body;
            const float width=canvas.measure(canvas.user,title,size);
            widget(titleBoard,240,26);
            canvas.text(canvas.user,title,(480.f-width)*.5f,16.f,size,white);
        }
        if(!overview)canvas.text(canvas.user,"Dirtmouth",178+shift.x,(quick?77.f:78.f)+shift.y,Size::Body,white);
        if(zoomed)canvas.text(canvas.user,"King's Pass",103+shift.x,82+shift.y,Size::Body,white);
    }
}
}
