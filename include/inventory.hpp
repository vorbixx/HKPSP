#pragma once
#include "hero.hpp"

namespace hk3ds {
class HudRenderer;
enum class InventoryTab { Inventory, Charms, Map };
enum class InventoryItem { Nail, Focus, Cloak, Claw, Wings, ShadeCloak, CityCrest, Spirit, Dive, Shriek, Count };
struct InventorySnapshot {
    Vitals vitals{};
    Abilities abilities{};
    unsigned geo=0;
    bool cityCrest=false;
    // Zero = unowned, one = base spell, two = upgraded. Gameplay supplies these
    // when spell acquisition is implemented; never infer ownership from SOUL.
    unsigned spirit=0,dive=0,shriek=0;
    bool owned(InventoryItem item) const {
        switch(item){
        case InventoryItem::Nail: case InventoryItem::Focus:return true;
        case InventoryItem::Cloak:return abilities.dash;
        case InventoryItem::Claw:return abilities.wallJump;
        case InventoryItem::Wings:return abilities.doubleJump;
        case InventoryItem::ShadeCloak:return abilities.shadeDash;
        case InventoryItem::CityCrest:return cityCrest;
        case InventoryItem::Spirit:return spirit!=0;
        case InventoryItem::Dive:return dive!=0;
        case InventoryItem::Shriek:return shriek!=0;
        default:return false;
        }
    }
};
// The Charms task can bind its page without owning the shell or the input loop.
// draw runs on the bottom C2D target, inside a frame, below the tab bar.
// input runs before simulation submission: Jump = A/confirm, Attack = X;
// B closes the shell, L/R change pages. draw must not mutate gameplay state.
struct InventoryPage {
    void* context=nullptr;
    void (*input)(void*,const Input&)=nullptr;
    void (*draw)(void*,HudRenderer&,const InventorySnapshot&)=nullptr;
};
class InventoryScreen {
public:
    InventoryPage charms{};
    bool opened() const {return opened_;}
    InventoryTab tab() const {return tab_;}
    InventoryItem selected() const {return selected_;}
    void toggle() {opened_=!opened_;}
    void close() {opened_=false;}
    // The map is the passive gameplay page; only detail pages capture input.
    void selectTab(InventoryTab tab) {tab_=tab;opened_=tab!=InventoryTab::Map;}
    void returnToMap() {selectTab(InventoryTab::Map);}
    void cycleBottomPage() {
        selectTab(tab_==InventoryTab::Map?InventoryTab::Inventory:
                  tab_==InventoryTab::Inventory?InventoryTab::Charms:InventoryTab::Map);
    }
    bool touchBottomTab(unsigned x,unsigned y) {
        if(y>=40||x>=320)return false;
        selectTab(x<107?InventoryTab::Map:x<214?InventoryTab::Inventory:InventoryTab::Charms);
        return true;
    }
    void changeTab(int direction) {
        selectTab(static_cast<InventoryTab>((static_cast<int>(tab_)+direction+3)%3));
    }
    void navigate(int direction,const InventorySnapshot& snapshot) {
        constexpr int count=static_cast<int>(InventoryItem::Count);
        if(!direction)return;
        int slot=(static_cast<int>(selected_)+direction%count+count)%count;
        const int step=direction>0?1:-1;
        for(int i=0;i<count;++i,slot=(slot+step+count)%count){
            selected_=static_cast<InventoryItem>(slot);
            if(snapshot.owned(selected_))break;
        }
    }
    void validate(const InventorySnapshot& snapshot) {
        if(!snapshot.owned(selected_))navigate(1,snapshot);
    }
private:
    bool opened_=false;
    InventoryTab tab_=InventoryTab::Inventory;
    InventoryItem selected_=InventoryItem::Nail;
};
}
