#pragma once
#include "ui.hpp"

namespace hk3ds {
// Merchant adapters supply stock; this component owns all shop presentation.
struct ShopMenuItem {
    const char* name;
    const char* description;
    const char* icon;
    unsigned price;
    bool owned=false;
};
struct ShopMenuView {
    const char* merchant;
    const ShopMenuItem* stock;
    unsigned count,selected,geo;
    bool confirm=false,yes=true;
    unsigned page=0;
    const char* message="";
};
struct ShopMenuCanvas {
    ui::Canvas body,headings;
    void* user;
    // "Geo" selects the resident currency art; other keys belong to stock.
    void (*icon)(void*,const char*,ui::Rect);
    void (*corners)(void*,ui::Rect)=nullptr;
};
// Top screen: item name, price and paged description. Bottom screen: the old Iselda-style
// window with the stock list and its hint lines (no plate, no button boxes).
ui::PageResult drawShopMenuTop(ShopMenuCanvas,const ShopMenuView&);
ui::PageResult drawShopMenuBottom(ShopMenuCanvas,const ShopMenuView&);
}
