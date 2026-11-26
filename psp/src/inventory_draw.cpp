#include "text_pack.hpp"
#include "../include/inventory_draw.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace hkpsp {
namespace {
using inventorylayout::Rect;
// Source strings: src/inventory_renderer.cpp:13-22. Never infer spell ownership
// from SOUL; the caller supplies the actual gameplay snapshot.
struct Detail {const char* icon;const char* name;const char* description;};
Detail itemDetail(hk3ds::InventoryItem item,const hk3ds::InventorySnapshot& s) {
    const char* icons[]={"Nail","Focus","Cloak","Claw","Wings","ShadeCloak","CityCrest",
        s.spirit>1?"Spirit2":"Spirit",s.dive>1?"Dive2":"Dive",s.shriek>1?"Shriek2":"Shriek"};
    const char* names[]={"Old Nail","Focus","Mothwing Cloak","Mantis Claw","Monarch Wings","Shade Cloak","City Crest",
        s.spirit>1?"Shade Soul":"Vengeful Spirit",s.dive>1?"Descending Dark":"Desolate Dive",
        s.shriek>1?"Abyss Shriek":"Howling Wraiths"};
    constexpr const char* descriptions[]={"@UI/INV_DESC_NAIL1","Collect SOUL to restore your health.",
        "Cloak that allows the wearer to dash.","Claw that allows the wearer to climb walls.",
        "Wings that allow a second jump in mid-air.","Cloak that allows dashing through enemies.",
        "An emblem of the Hallownest capital city.","Conjure a spirit to strike foes ahead.",
        "Strike the ground with concentrated SOUL.","@UI/INV_DESC_SPELL_SCREAM1"};
    const auto i=static_cast<unsigned>(item);return {icons[i],names[i],hk3ds::text::resolve(descriptions[i])};
}
void label(InventoryLabels labels,const char* text,Rect bounds,bool heading=false) {
    if(labels.draw)labels.draw(labels.context,text,bounds,heading);
}
}
InventoryArt::InventoryArt(hk3ds::AnimationBank bank,unsigned page):bank_(std::move(bank)),page_(page) {
    for(const auto* name:{"Nail","Focus","Mask","Vessel","Geo","Cursor","Divider",
                         "NotchEmpty","NotchUsed","NotchOver","CharmSlot"})
        if(!bank_.find(name))throw std::runtime_error(std::string("Missing inventory icon: ")+name);
    for(const auto& d:hk3ds::charmDefinitions){
        char name[24];std::snprintf(name,sizeof name,"Charm%u",d.id);
        // HK3DS's converted package has no Defender's Crest (id 10) sprite;
        // its renderer leaves that icon empty as well.
        if(d.id!=10&&!bank_.find(name))throw std::runtime_error(std::string("Missing inventory icon: ")+name);
    }
}
void InventoryArt::icon(DrawList& list,const char* name,Rect bounds,float alpha,bool flipX,bool flipY)const {
    const auto* clip=bank_.find(name);
    if(!clip||!clip->count)throw std::runtime_error(std::string("Missing inventory icon: ")+name);
    const auto& s=bank_.sprites.at(bank_.frames.at(clip->first));
    const float scale=inventorylayout::fit(float(s.width),float(s.height),bounds);
    const float w=s.width*scale,h=s.height*scale;
    const float x=std::round(bounds.x+(bounds.w-w)/2),y=std::round(bounds.y+(bounds.h-h)/2);
    if(!inventorylayout::inside({x,y,w,h}))throw std::runtime_error("Inventory sprite outside screen");
    Quad q;q.page=page_+s.page;q.clut=s.clut;q.rgba[3]=alpha;
    q.u0=float(s.x+(flipX?s.width:0));q.u1=float(s.x+(flipX?0:s.width));
    q.v0=float(s.y+(flipY?s.height:0));q.v1=float(s.y+(flipY?0:s.height));
    q.positions={{{x,y},{x+w,y},{x+w,y+h},{x,y+h}}};list.quads.push_back(q);
}
void InventoryArt::selection(DrawList& list,Rect r)const {
    const float corner=std::min({16.f,r.w/2,r.h/2});
    icon(list,"Cursor",{r.x,r.y,corner,corner});
    icon(list,"Cursor",{r.x+r.w-corner,r.y,corner,corner},1,true);
    icon(list,"Cursor",{r.x,r.y+r.h-corner,corner,corner},1,false,true);
    icon(list,"Cursor",{r.x+r.w-corner,r.y+r.h-corner,corner,corner},1,true,true);
}
void InventoryArt::append(DrawList& list,const hk3ds::InventorySnapshot& snapshot,
                         const hk3ds::CharmInventory& charms,hk3ds::InventoryItem selected,
                         unsigned charm,InventoryLabels labels)const {
    if(inventoryPreview==InventoryPreview::Off)return;
    if(!charms.valid()||charm>=hk3ds::charmDefinitions.size()||
       static_cast<unsigned>(selected)>=static_cast<unsigned>(hk3ds::InventoryItem::Count))
        throw std::runtime_error("Invalid inventory presentation state");
    // The original inventory completely covers the world. FadeBlack already
    // implements the untextured opaque black pass on both backends.
    Quad background;background.blend=Blend::FadeBlack;background.rgba={0,0,0,1};
    background.positions={{{0,0},{480,0},{480,272},{0,272}}};list.quads.push_back(background);
    using namespace inventorylayout;
    icon(list,"Divider",{16,34,280,6});icon(list,"Divider",{310,34,154,6});
    if(inventoryPreview==InventoryPreview::Items){
        label(labels,"Inventory",heading,true);
        icon(list,"Mask",health);icon(list,"Vessel",soul);icon(list,"Geo",geo);
        char value[48];std::snprintf(value,sizeof value,"%d / %d",snapshot.vitals.health,snapshot.vitals.maxHealth);
        label(labels,value,{54,54,84,20});
        std::snprintf(value,sizeof value,"%u",snapshot.geo);label(labels,value,{269,54,32,20});
        unsigned slot=1;
        for(unsigned i=0;i<static_cast<unsigned>(hk3ds::InventoryItem::Count);++i){
            const auto item=static_cast<hk3ds::InventoryItem>(i);if(!snapshot.owned(item))continue;
            const Rect bounds=inventorylayout::item(item==hk3ds::InventoryItem::Nail?0:slot++);
            icon(list,itemDetail(item,snapshot).icon,bounds);
            if(item==selected)selection(list,{bounds.x-3,bounds.y-3,bounds.w+6,bounds.h+6});
        }
        const auto d=itemDetail(selected,snapshot);
        label(labels,d.name,{310,46,154,48},true);
        label(labels,d.description,{310,102,154,146});
    }else{
        label(labels,"Charms",heading,true);
        unsigned slot=0;
        for(const auto& d:hk3ds::charmDefinitions)if(charms.wears(d.id)){
            char name[24];std::snprintf(name,sizeof name,"Charm%u",d.id);
            if(bank_.find(name))icon(list,name,equippedSlot(slot));
            ++slot;
        }
        // Preserve empty equipped slots even in the unmodified Tutorial state.
        for(;slot<charms.notches;++slot)icon(list,"CharmSlot",equippedSlot(slot));
        for(unsigned n=0;n<std::max(unsigned(charms.notches),charms.used());++n)
            icon(list,n>=charms.notches?"NotchOver":n<charms.used()?"NotchUsed":"NotchEmpty",notch(n));
        for(unsigned i=0;i<hk3ds::charmDefinitions.size();++i){
            const auto& d=hk3ds::charmDefinitions[i];const Rect r=inventorylayout::charm(i);
            icon(list,"CharmSlot",r);
            if(charms.has(d.id)){
                char name[24];std::snprintf(name,sizeof name,"Charm%u",d.id);
                if(bank_.find(name))icon(list,name,r);
            }
            if(i==charm)selection(list,r);
        }
        const auto& d=hk3ds::charmDefinitions[charm];
        label(labels,d.name,{310,46,154,48},true);
        char value[64];std::snprintf(value,sizeof value,"Cost %u   Notches %u/%u",d.cost,charms.used(),unsigned(charms.notches));
        label(labels,value,{310,98,154,36});
        label(labels,charms.has(d.id)?hk3ds::text::resolve(d.description):"Not collected",{310,140,154,108});
        label(labels,"Rest at a bench to equip charms",footer);
    }
}
}
