#pragma once
#include "draw_list.hpp"
#include "shop_menu.hpp"

namespace hk3ds { class TutorialSession; }
namespace hkpsp {

struct SlyShopCanvas {
    hk3ds::ui::Canvas text;
    void (*art)(void*,const char*,hk3ds::ui::Rect,float);
    // Selection wing, 16 px square at (x,y); right=true points right.
    void (*wing)(void*,float,float,bool);
};
unsigned drawSlyShopMenu(SlyShopCanvas,const hk3ds::ShopMenuView&);
constexpr hk3ds::Vec2 slyShopCamera(hk3ds::Vec2 camera,bool open){
    return open?hk3ds::Vec2{14.6f,9.9f}:camera;
}

class SlyShopArt {
    AnimationAtlas actor_;
public:
    AnimationAtlas icons;
    void clear(hk3ds::TutorialSession&);
    void load(Platform&,std::vector<Texture>&,hk3ds::TutorialSession&);
    // Room_mapper: the ornament page only, for Iselda's shop.
    void loadOrnaments(Platform&,std::vector<Texture>&);
    void append(DrawList&,const hk3ds::TutorialSession&,hk3ds::Vec2 camera)const;
};
}
