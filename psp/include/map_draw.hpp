#pragma once
#include "draw_list.hpp"
#include "platform.hpp"
#include "map_view.hpp"
#include "ui.hpp"

namespace hkpsp {
enum class MapView { Closed, Items, Inventory, InventoryZoom, Quick, Charms };
class MapOverlay {
    MapView view_=MapView::Closed;
    hk3ds::Vec2 pan_{};
public:
    MapView view()const{return view_;}
    hk3ds::Vec2 pan()const{return pan_;}
    bool showsMap()const{return view_==MapView::Inventory||view_==MapView::InventoryZoom||view_==MapView::Quick;}
    bool pauses()const{return view_!=MapView::Closed&&view_!=MapView::Quick;}
    void update(const UiInput&,bool allowed,bool ownsMap,bool inCrossroads);
};
class MapAtlas {
    std::string path_;
    unsigned page_;
    bool loaded_=false;
    static Texture descriptor();
public:
    MapAtlas(Platform&,std::vector<Texture>&);
    unsigned page()const{return page_;}
    void setVisible(Platform&,std::vector<Texture>&,bool);
};
void appendMap(DrawList&,hk3ds::ui::Canvas,unsigned page,MapView,const hk3ds::CartographySave&,
               std::size_t current,hk3ds::Vec2 hero,bool compass,hk3ds::Vec2 pan={});
}
