#pragma once
namespace hk3ds {
// Shared, platform-independent row navigation for title and pause menus.
struct MenuNavigation {
    unsigned selected=0;
    void move(int direction,unsigned count) {
        if(!count){selected=0;return;}
        selected=(selected+count+(direction<0?-1:direction>0?1:0))%count;
    }
};
}
