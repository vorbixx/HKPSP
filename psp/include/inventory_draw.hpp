#pragma once
#include "draw_list.hpp"
#include "inventory.hpp"
#include "charms.hpp"
#include "inventory_layout.hpp"

namespace hkpsp {
// Only test-mode.cfg selects these pages until the menu input router lands.
enum class InventoryPreview {Off,Items,Charms};
inline InventoryPreview inventoryPreview=InventoryPreview::Off;
// Canvas adapter hook. Labels retain source strings and final-size rectangles;
// no local UI text engine: psp-text owns measurement, wrapping and pagination.
struct InventoryLabels {
    void* context=nullptr;
    void (*draw)(void*,const char*,inventorylayout::Rect,bool heading)=nullptr;
};
class InventoryArt {
    hk3ds::AnimationBank bank_;
    unsigned page_=0;
    void icon(DrawList&,const char*,inventorylayout::Rect,float alpha=1,bool flipX=false,bool flipY=false)const;
    void selection(DrawList&,inventorylayout::Rect)const;
public:
    InventoryArt(hk3ds::AnimationBank bank,unsigned firstPage);
    const hk3ds::AnimationBank& bank()const{return bank_;}
    void append(DrawList&,const hk3ds::InventorySnapshot&,const hk3ds::CharmInventory&,
                hk3ds::InventoryItem selected=hk3ds::InventoryItem::Nail,unsigned charm=0,
                InventoryLabels labels={})const;
};
}
