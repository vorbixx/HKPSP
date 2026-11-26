#pragma once
#include "inventory.hpp"
#include "ui.hpp"
#include "renderer.hpp"

namespace hk3ds {
class InventoryRenderer {
public:
    bool load(const char* directory);
    void release() {renderer_.release();bank_={};}
    void draw(C3D_RenderTarget* target,InventoryScreen& screen,
              const InventorySnapshot& snapshot,HudRenderer& hud) const;
    void drawDetail(const InventoryScreen&,const InventorySnapshot&,HudRenderer&,const char* area=nullptr) const;
    bool drawIconFit(const char* name,ui::Rect bounds,float alpha=1,bool flipX=false,bool flipY=false) const;
    void drawSelection(ui::Rect bounds) const;
    void drawTabs(InventoryTab selected,HudRenderer& hud) const;
private:
    void icon(const char* name,float x,float y,float scale=1) const;
    AnimationBank bank_;
    HeroRenderer renderer_;
};
}
