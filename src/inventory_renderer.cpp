#include "text_pack.hpp"
#include <cstring>
#include "inventory_renderer.hpp"
#include "ui_window.hpp"
#include "hud_renderer.hpp"
#include "inventory_layout.hpp"
#include <cstdio>
#include <algorithm>

namespace hk3ds {
namespace {
struct ItemDetail {const char* icon;const char* name;const char* description;};
ItemDetail itemDetail(InventoryItem item,const InventorySnapshot& snapshot) {
    const char* icons[]={"Nail","Focus","Cloak","Claw","Wings","ShadeCloak","CityCrest",
        snapshot.spirit>1?"Spirit2":"Spirit",snapshot.dive>1?"Dive2":"Dive",snapshot.shriek>1?"Shriek2":"Shriek"};
    const char* names[]={"Old Nail","Focus","Mothwing Cloak","Mantis Claw","Monarch Wings","Shade Cloak","City Crest",
        snapshot.spirit>1?"Shade Soul":"Vengeful Spirit",snapshot.dive>1?"Descending Dark":"Desolate Dive",
        snapshot.shriek>1?"Abyss Shriek":"Howling Wraiths"};
    constexpr const char* descriptions[]={"@UI/INV_DESC_NAIL1","Collect SOUL to restore your health.",
        "Cloak that allows the wearer to dash.","Claw that allows the wearer to climb walls.",
        "Wings that allow a second jump in mid-air.","Cloak that allows dashing through enemies.",
        "An emblem of the Hallownest capital city.","Conjure a spirit to strike foes ahead.",
        "Strike the ground with concentrated SOUL.","@UI/INV_DESC_SPELL_SCREAM1"};
    const auto i=static_cast<unsigned>(item);
    return {icons[i],names[i],hk3ds::text::resolve(descriptions[i])};
}
}
bool InventoryRenderer::load(const char* directory)
{
    char path[256];std::snprintf(path,sizeof(path),"%s/inventory.bin",directory);
    return bank_.load(path)&&renderer_.load(directory,bank_,"inventory-page-");
}
void InventoryRenderer::icon(const char* name,float x,float y,float scale) const
{
    AnimationPlayer player;player.play(bank_.find(name));
    renderer_.drawAnimation(player,bank_,x,y,1,scale,scale);
}
bool InventoryRenderer::drawIconFit(const char* name,ui::Rect r,float alpha,bool flipX,bool flipY) const {
    AnimationPlayer player;player.play(bank_.find(name));
    const auto* f=player.current(bank_);if(!f)return false;
    const float width=f->right-f->left,height=f->top-f->bottom;
    if(width<=0||height<=0||r.w<=0||r.h<=0)return false;
    const float fit=std::min(r.w/width,r.h/height);
    const float scale=std::strncmp(name,"Charm",5)==0?std::min(1.f,fit):fit;
    const float sx=flipX?-scale:scale,sy=flipY?-scale:scale;
    renderer_.drawAnimation(player,bank_,r.x+r.w/2-(f->left+f->right)*sx/2,
        r.y+r.h/2+(f->top+f->bottom)*sy/2,1,sx,sy,alpha);
    return true;
}
void InventoryRenderer::drawSelection(ui::Rect r) const {
    const float corner=std::min({16.f,r.w/2,r.h/2});
    if(corner<=0)return;
    drawIconFit("Cursor",{r.x,r.y,corner,corner});
    drawIconFit("Cursor",{r.x+r.w-corner,r.y,corner,corner},1,true);
    drawIconFit("Cursor",{r.x,r.y+r.h-corner,corner,corner},1,false,true);
    drawIconFit("Cursor",{r.x+r.w-corner,r.y+r.h-corner,corner,corner},1,true,true);
}
void InventoryRenderer::drawTabs(InventoryTab selected,HudRenderer& hud) const
{
    const auto line=C2D_Color32(118,125,145,160);
    C2D_DrawRectSolid(0,0,0,320,40,C2D_Color32(8,10,18,250));
    const char* labels[]={"Map","Inventory","Charms"};
    const InventoryTab tabs[]={InventoryTab::Map,InventoryTab::Inventory,InventoryTab::Charms};
    for(unsigned i=0;i<3;++i){
        const float center=53.f+107.f*i;
        hud.inventoryText(labels[i],center,22,selected==tabs[i]?1:.75f);
        if(selected==tabs[i])C2D_DrawLine(center-35,35,line,center+35,35,line,1,0);
    }
    C2D_DrawLine(0,39,line,320,39,line,1,0);
}
void InventoryRenderer::draw(C3D_RenderTarget* target,InventoryScreen& screen,
                             const InventorySnapshot& snapshot,HudRenderer& hud) const
{
    if(!screen.opened())return;
    C2D_SceneBegin(target);C2D_ViewReset();
    using namespace inventorylayout;
    // Approximation: compact LCD layout, retaining source icons and typeface.
    ui::window(hud.uiCanvas());
    const auto line=C2D_Color32(118,125,145,160);
    C2D_DrawLine(x(20),208,line,x(380),208,line,1,0);
    drawTabs(screen.tab(),hud);
    const ui::Action actions[]={{"SELECT","Page"},{"D-Pad","Select"}};
    if(screen.tab()!=InventoryTab::Charms||!screen.charms.draw)ui::promptBar(hud.uiCanvas(),actions,2);
    if(screen.tab()==InventoryTab::Charms){
        if(screen.charms.draw)screen.charms.draw(screen.charms.context,hud,snapshot);
        else {
            hud.inventoryText("Charms",center,100);
            hud.inventoryText("Rest at a bench to equip charms",center,132,.65f);
        }
        return;
    }
    if(screen.tab()==InventoryTab::Map){
        hud.inventoryText("Map of Hallownest",center,92);
        icon("Divider",center,111);
        hud.inventoryText("Your map is on the lower screen",center,146);
        return;
    }
    char value[48];
    drawIconFit("Geo",{244,44,24,24});
    std::snprintf(value,sizeof(value),"%u",snapshot.geo);hud.inventoryText(value,286,66);
    drawIconFit("Mask",{20,44,36,36});
    std::snprintf(value,sizeof(value),"%d / %d",snapshot.vitals.health,snapshot.vitals.maxHealth);
    hud.inventoryText(value,92,67);
    drawIconFit("Vessel",{152,44,36,36});
    unsigned slot=0;
    for(unsigned i=0;i<static_cast<unsigned>(InventoryItem::Count);++i){
        const auto item=static_cast<InventoryItem>(i);if(!snapshot.owned(item))continue;
        ui::Rect bounds{};
        if(item==InventoryItem::Nail)bounds={28,86,44,88};
        else {bounds={94+40.f*(slot%5),86+40.f*(slot/5),32,32};++slot;}
        drawIconFit(itemDetail(item,snapshot).icon,bounds);
        if(screen.selected()==item)drawSelection({bounds.x-7,bounds.y-7,bounds.w+14,bounds.h+14});
    }
    ui::wrappedText(hud.uiCanvas(),itemDetail(screen.selected(),snapshot).name,{96,168,208,24},ui::Size::Heading);
}
}

namespace hk3ds {
void InventoryRenderer::drawDetail(const InventoryScreen& screen,const InventorySnapshot& snapshot,HudRenderer& hud,const char* area) const {
    if(!screen.opened()||screen.tab()==InventoryTab::Charms)return;
    const auto c=hud.uiCanvas();
    if(screen.tab()==InventoryTab::Map){
        ui::detail(c,"Map of Hallownest",area,"Explore to fill in the map. The lower screen shows the current area.",{20,80,360,152});
        return;
    }
    const auto selected=itemDetail(screen.selected(),snapshot);
    ui::panel(c,{0,0,400,240});
    drawIconFit(selected.icon,{20,24,108,188});
    auto content=c;content.art=nullptr;
    ui::detail(content,selected.name,nullptr,selected.description,{148,8,244,224},false);
}
}
