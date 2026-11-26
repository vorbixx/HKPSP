#include "shop_menu.hpp"
#include "ui_window.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace hk3ds {
namespace {
// Layout and style of the Iselda shop as it was before the 2026-11-03 ui-consistency
// standardisation (task/iselda-visual HudRenderer::drawShop): a dark window with ornaments,
// a left title, a centred Geo label, three 48 px rows, and hint lines inside the window.
constexpr unsigned rowsShown=3;
const ShopMenuItem* selected(const ShopMenuView& v) {
    return v.stock&&v.count?&v.stock[std::min(v.selected,v.count-1)]:nullptr;
}
void icon(ShopMenuCanvas c,const char* name,ui::Rect r) {
    if(c.icon)c.icon(c.user,name,r);
}
// Source shop labels contain desktop hard wraps. Retain paragraph breaks and
// reflow the wording at native LCD size instead of shrinking the font.
void description(char* out,unsigned capacity,const char* source) {
    unsigned n=0;
    if(source)for(unsigned i=0;source[i]&&n+1<capacity;++i){
        if(source[i]=='\n'&&source[i+1]!='\n'&&(i==0||source[i-1]!='\n'))out[n++]=' ';
        else out[n++]=source[i];
    }
    out[n]=0;
}
}
// Top screen: the selected item's name, price and description (the old shop showed these
// above the window), paged with the same page counter text.
ui::PageResult drawShopMenuTop(ShopMenuCanvas c,const ShopMenuView& v) {
    const auto* item=selected(v);
    c.body.fill(c.body.user,{24,0,352,204},0xff000000);
    if(c.corners)c.corners(c.user,{24,0,352,204});
    if(!item){ui::label(c.body,"Sold out",200,70,true);return {0,1};}
    char line[96];
    std::snprintf(line,sizeof(line),"%s - %u Geo",item->name,item->price);
    ui::label(c.body,line,200,66,true);
    char body[1024];description(body,sizeof(body),item->description);
    const auto pages=ui::pagedText(c.body,body,{40,80,320,100},v.page,ui::Size::Heading);
    if(pages.pages>1){
        std::snprintf(line,sizeof(line),"Page %u/%u - L/R",pages.page+1,pages.pages);
        ui::label(c.body,line,200,196,true,ui::secondary);
    }
    return pages;
}
ui::PageResult drawShopMenuBottom(ShopMenuCanvas c,const ShopMenuView& v) {
    c.body.fill(c.body.user,{0,0,320,240},ui::bottomBackgroundColor);
    char text[96];std::snprintf(text,sizeof(text),"%u Geo",v.geo);
    ui::window(c.body,v.merchant,text);
    const auto* item=selected(v);
    if(!item){
        ui::label(c.body,"Sold out",160,110,true);
        const char* lines[]={"B: Exit"};ui::hintLines(c.body,lines,1);
        return {0,1};
    }
    if(v.confirm){
        ui::label(c.body,item->name,160,78,true);
        std::snprintf(text,sizeof(text),"%u Geo",item->price);
        ui::label(c.body,text,160,102,true,ui::secondary);
        ui::label(c.body,"Buy this item?",160,134,true);
        ui::label(c.body,v.yes?"> Yes":"Yes",110,164,true,v.yes?ui::white:ui::secondary);
        ui::label(c.body,v.yes?"No":"> No",210,164,true,v.yes?ui::secondary:ui::white);
        const char* lines[]={"A: Confirm     B: Back"};ui::hintLines(c.body,lines,1);
        return {0,1};
    }
    const unsigned focus=std::min(v.selected,v.count-1);
    const unsigned first=v.count>rowsShown?(focus>1?std::min(focus-1,v.count-rowsShown):0):0;
    for(unsigned row=0;row<rowsShown&&first+row<v.count;++row){
        const unsigned index=first+row;const auto& entry=v.stock[index];
        const float y=ui::rowFirstBaseline+row*ui::rowStep;
        std::snprintf(text,sizeof(text),"%u",entry.price);
        ui::selectionRow(c.body,y,index==focus,entry.name,text,true);
        icon(c,entry.icon,{19,y-20,40,40});
        icon(c,"Geo",{294,y-22,16,20});
        if(entry.owned)ui::label(c.body,"Owned",78,y+16,true);
    }
    // Sly's list moves with Up/Down and his text pages with Left/Right.
    const char* lines[]={"U/D: Stock   L/R: Read","A: Buy     B: Exit"};
    ui::hintLines(c.body,lines,2);
    return {0,1};
}
}
