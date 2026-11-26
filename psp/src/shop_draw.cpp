#include "shop_draw.hpp"
#include "text_pack.hpp"
#include "text_draw.hpp"
#include "../include/platform.hpp"
#include "tutorial.hpp"
#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace hkpsp {
namespace {
using namespace hk3ds::ui;
constexpr float centre=320;
constexpr unsigned visibleRows=4;
constexpr Rect descriptionRect{309,86,94,96};
constexpr float pageTop=186;
constexpr float hintTop=205;

void centered(Canvas canvas,const char* label,float x,float y,Size size,std::uint32_t color=white){
    canvas.text(canvas.user,label,x-canvas.measure(canvas.user,label,size)*.5f,y,size,color);
}
void hints(Canvas text,bool confirm){
    const char* cancel=confirm?"Cancel":"Exit";
    const char* accept=confirm?"Confirm":"Buy";
    constexpr float left=246,right=395;
    text.fill(text.user,{left,hintTop+4,10,10},white);
    text.fill(text.user,{left+1,hintTop+5,8,8},0xff000000u);
    text.text(text.user,cancel,left+15,hintTop,Size::Compact,white);
    text.text(text.user,accept,right-15-text.measure(text.user,accept,Size::Compact),hintTop,Size::Compact,white);
    centered(text,"X",right-5,hintTop,Size::Compact);
}
void price(SlyShopCanvas canvas,unsigned geo,float x,float y,float opacity=1){
    char digits[12];std::snprintf(digits,sizeof digits,"%u",geo);
    canvas.art(canvas.text.user,"Coin Idle",{x,y+2,10,10},opacity);
    canvas.text.text(canvas.text.user,digits,x+13,y,Size::Compact,opacity==1?white:0xff9c9894u);
}
AnimationAtlas loadAtlas(Platform& platform,std::vector<Texture>& textures,const char* scene,const char* stem){
    const std::string prefix=std::string("rooms/")+scene+"/"+stem;
    auto bank=readAnimation(platform.path(prefix+"-visual.bin"));
    if(bank.pageCount!=1)throw std::runtime_error("Sly shop page budget");
    const auto page=unsigned(textures.size());
    textures.push_back(readTexture(platform.path(prefix+"-page-0.hpt")));
    return {std::move(bank),page};
}
}

unsigned drawSlyShopMenu(SlyShopCanvas canvas,const hk3ds::ShopMenuView& view){
    const auto text=canvas.text;
    text.fill(text.user,{226,45,200,187},0xff000000u);
    canvas.art(text.user,"Sly Shop Top",{228,33,184,40},1);
    canvas.art(text.user,"Sly Shop Bottom",{227,201,186,49},1);
    canvas.art(text.user,"Sly Shop Figurehead",{306,16,27,34},1);
    hints(text,view.confirm);
    if(!view.stock||!view.count)return 1;

    const unsigned selected=std::min(view.selected,view.count-1);
    const auto& item=view.stock[selected];
    centered(text,item.name,centre,60,Size::Heading);
    if(view.confirm){
        canvas.art(text.user,item.icon,{centre-17,81,34,34},1);
        price(canvas,item.price,centre-18,119);
        centered(text,"Purchase this item?",centre,139,Size::Body);
        for(unsigned choice=0;choice<2;++choice){
            const float top=162.f+20.f*choice;
            const char* label=choice==0?"Yes":"No";
            if((choice==0)==view.yes){
                // Same bar and wing gaps as the toll Yes/No box (text_draw.cpp choiceBox): the bar hugs the
                // label (1 px left, 21 px right of it) and the wings stand outside it (17 px left, 21 px right).
                const float width=text.measure(text.user,label,Size::Compact);
                const float left=centre-width*.5f;
                text.fill(text.user,{left-1,top+2,width+22,14},0x50ffffffu);
                canvas.wing(text.user,left-17,top+2,false);
                canvas.wing(text.user,left+width+21,top+2,true);
            }
            centered(text,label,centre,top,Size::Compact);
        }
        return 1;
    }

    const unsigned first=selected/visibleRows*visibleRows;
    const unsigned end=std::min(first+visibleRows,view.count);
    const float start=109.f-(float(end-first)-2.f)*16.f;
    for(unsigned index=first;index<end;++index){
        const auto& row=view.stock[index];
        const float top=start+float(index-first)*27;
        if(index==selected)text.fill(text.user,{230,top,71,26},0x38ffffffu);
        const float opacity=row.price<=view.geo&&!row.owned?1.f:.52f;
        canvas.art(text.user,row.icon,{235,top+2,22,22},opacity);
        price(canvas,row.price,262,top+6,opacity);
    }
    if(view.count>visibleRows){
        char pages[24];std::snprintf(pages,sizeof pages,"%u/%u",first/visibleRows+1,(view.count-1)/visibleRows+1);
        centered(text,pages,265,pageTop,Size::Compact,secondary);
    }

    char description[512]{};
    if(item.description)for(unsigned index=0;item.description[index]&&index+1<sizeof description;++index){
        const char ch=item.description[index];
        const bool wrap=ch=='\n'&&item.description[index+1]!='\n'&&(!index||item.description[index-1]!='\n');
        description[index]=wrap?' ':ch;
    }
    const auto page=pagedText(text,description,descriptionRect,view.page,Size::Compact);
    if(page.pages>1){
        char pages[24];std::snprintf(pages,sizeof pages,"< %u/%u >",page.page+1,page.pages);
        centered(text,pages,descriptionRect.x+descriptionRect.w*.5f,pageTop,Size::Compact,secondary);
    }
    return page.pages;
}

void TextCanvas::slyShop(const hk3ds::TutorialSession& game){
    const auto& shop=game.slyShop;
    std::array<hk3ds::ShopMenuItem,hk3ds::slyItems.size()> stock{};
    for(unsigned index=0;index<shop.count;++index){
        const auto& item=hk3ds::slyItems[shop.stock[index]];
        stock[index]={item.name,hk3ds::text::resolve(item.description),item.flag,item.price};
    }
    const hk3ds::ShopMenuView view{"Sly",stock.data(),shop.count,shop.selected,game.geo,
                                  shop.confirm,shop.yes,shop.textPage,shop.message};
    const auto pages=drawShopView(view);
    if(!view.confirm)shop.setVisibleTextPages(pages);
}

// Iselda's purchase step (Prompt) is the same single-screen shop as Sly's, with her one-item stock
// (hk3ds::Cartographer::shopItem, price 40). Her map has no PSP item icon: the 3DS draws none either.
void TextCanvas::iseldaShop(const hk3ds::Cartographer& npc,unsigned geo){
    const auto item=npc.shopItem();
    drawShopView(npc.shopMenu(item,geo));
}

unsigned TextCanvas::drawShopView(const hk3ds::ShopMenuView& view){
    return drawSlyShopMenu({canvas(),[](void* user,const char* name,Rect rect,float opacity){
        // A null key (Iselda's map) draws nothing, as the 3DS shopIcon callback does.
        if(!name)return;
        auto& self=*static_cast<TextCanvas*>(user);
        const auto* atlas=self.shopIcons&&self.shopIcons->bank.find(name)?self.shopIcons:nullptr;
        const auto& bank=atlas?atlas->bank:self.art->bank;
        const auto* clip=bank.find(name);
        if(!clip||!clip->count)throw std::runtime_error("Missing Sly shop art");
        const auto& frame=bank.sprites.at(bank.frames.at(clip->first));
        const float scale=std::min({rect.w/float(frame.width),rect.h/float(frame.height),1.f});
        const float width=frame.width*scale,height=frame.height*scale;
        const float x=rect.x+(rect.w-width)*.5f,y=rect.y+(rect.h-height)*.5f;
        Quad quad;
        quad.page=(atlas?atlas->page:self.art->firstPage)+frame.page;
        quad.clut=frame.clut;
        quad.u0=float(frame.x);quad.v0=float(frame.y);
        quad.u1=quad.u0+frame.width;quad.v1=quad.v0+frame.height;
        quad.rgba[3]=opacity;
        quad.positions={{{x,y},{x+width,y},{x+width,y+height},{x,y+height}}};
        self.list.quads.push_back(quad);
    },[](void* user,float x,float y,bool right){
        auto& self=*static_cast<TextCanvas*>(user);
        if(self.hasMenuArrowPage)appendMenuArrow(self.list,self.menuArrowPage,x,y,right);
    }},view);
}

void SlyShopArt::clear(hk3ds::TutorialSession& game){
    game.slyShopAnimation={};actor_={};icons={};
}
void SlyShopArt::load(Platform& platform,std::vector<Texture>& textures,hk3ds::TutorialSession& game){
    actor_=loadAtlas(platform,textures,"Room_shop","sly-actor");
    icons=loadAtlas(platform,textures,"Room_shop","sly-icons");
    for(const auto& item:hk3ds::slyItems)icons.bank.require(item.flag);
    for(const char* key:{"Sly Shop Top","Sly Shop Bottom","Sly Shop Figurehead"})icons.bank.require(key);
    game.slyShopAnimation.play(actor_.bank.require("Sly Idle"));
}
// Room_mapper (Iselda): only the shared ornament page; her stock has no item art.
void SlyShopArt::loadOrnaments(Platform& platform,std::vector<Texture>& textures){
    icons=loadAtlas(platform,textures,"Room_mapper","sly-icons");
    for(const char* key:{"Sly Shop Top","Sly Shop Bottom","Sly Shop Figurehead"})icons.bank.require(key);
}
void SlyShopArt::append(DrawList& list,const hk3ds::TutorialSession& game,hk3ds::Vec2 camera)const{
    if(!actor_.bank.pageCount||!game.slyShopVisible())return;
    ActorPose pose;
    pose.position={hk3ds::slyShopX,hk3ds::slyShopY};pose.scale={game.slyShop.artScale(),1};
    appendActor(list,actor_.bank,game.slyShopAnimation,pose,camera,actor_.page);
}
}
