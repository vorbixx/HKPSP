#include "menu_runtime.hpp"
#include "../include/platform.hpp"
#include "save.hpp"
#include "text_draw.hpp"
#include "text_pack_io.hpp"
#include "title_font_data.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace hkpsp {
void MenuRuntime::loadInput(const std::string& path){
    if(!path.empty()&&(!replay.load(path.c_str())||replay.benchmarkRoom()!=-1))
        throw std::runtime_error("Menu input replay invalid: "+path);
}
void MenuRuntime::loadSettings(const std::string& slotPath){
    if(slotPath.empty())return;
    settingsPath_=slotPath.substr(0,slotPath.find_last_of("/\\")+1)+"system.sav";
    state.pause.settings.load(settingsPath_.c_str());
}
void MenuRuntime::saveSettings(){
    state.pause.saveFailed=!settingsPath_.empty()&&!state.pause.settings.write(settingsPath_.c_str());
}
UiInput MenuRuntime::pollUi(Platform& platform){
    if(!replay.active())return platform.pollUi();
    // Reuse the original HKIR parser and HID positions; one record tick is now
    // one presentation, even while paused. Gameplay stays neutral on this tape.
    hk3ds::InputLatch unused;replay.sample(unused);
    const auto keys=replay.keys();unsigned held=0;
    constexpr unsigned masks[]={64,128,32,16,1,2,8,4,1024};
    constexpr unsigned buttons[]={UiUp,UiDown,UiLeft,UiRight,UiCircle,UiCross,UiStart,UiSelect,UiTriangle};
    for(unsigned i=0;i<9;++i)if(keys&masks[i])held|=buttons[i];
    return router.sample(held,1./30.);
}
namespace {
using namespace hk3ds::ui;
struct Widget {float u,v,width,height;};
constexpr Widget headingFleur{24,0,184,16},profileFrame{0,28,254,27},
    profileSelection{0,55,254,35},pauseTop{0,92,107,31},pauseBottom{112,92,76,17},
    audioHandle{192,92,15,32};
void widget(DrawList& list,unsigned page,Widget art,float left,float top){
    Quad quad;quad.page=page;
    quad.u0=art.u;quad.v0=art.v;quad.u1=art.u+art.width;quad.v1=art.v+art.height;
    const float right=left+art.width,bottom=top+art.height;
    quad.positions={{{left,top},{right,top},{right,bottom},{left,bottom}}};
    quad.order=list.nextOrder();list.quads.push_back(quad);
}
}
// Shared with the Cornifer Yes/No box (text_draw.cpp), so it sits outside the anonymous namespace.
void appendMenuArrow(DrawList& list,unsigned page,float x,float y,bool right){
    Quad arrow;arrow.page=page;arrow.u0=right?16:0;arrow.u1=right?0:16;
    arrow.v0=9;arrow.v1=25;
    arrow.positions={{{x,y},{x+16,y},{x+16,y+16},{x,y+16}}};
    arrow.order=list.nextOrder();list.quads.push_back(arrow);
}
namespace {
void audioSlider(DrawList& list,unsigned page,Canvas canvas,const char* label,float y,
                 unsigned value,bool selected){
    const auto color=selected?white:secondary;
    canvas.text(canvas.user,label,133.f,y,Size::Heading,color);
    Quad track;track.page=page;track.u1=1;track.v1=1;
    track.positions={{{264,y+6},{319,y+6},{319,y+7},{264,y+7}}};
    track.rgba[3]=.63f;track.order=list.nextOrder();list.quads.push_back(track);
    widget(list,page,audioHandle,257.f+5.5f*value,y-10.f);
    char number[4];
    std::snprintf(number,sizeof(number),"%u",value);
    canvas.text(canvas.user,number,335.f,y,Size::Compact,color);
    if(selected){
        appendMenuArrow(list,page,108.f,y,false);
        appendMenuArrow(list,page,358.f,y,true);
    }
}
void centered(Canvas canvas,const char* text,float y,Size size,std::uint32_t color=white){
    const float width=canvas.measure(canvas.user,text,size);
    canvas.text(canvas.user,text,(480.f-width)*.5f,y,size,color);
}
void heading(DrawList& list,unsigned page,Canvas canvas,const char* text,float y){
    centered(canvas,text,y,Size::Title);
    widget(list,page,headingFleur,148,y+19);
}
void row(DrawList& list,unsigned page,Canvas canvas,const char* label,float y,bool selected,
         Size size=Size::Heading){
    const float width=canvas.measure(canvas.user,label,size);
    const float left=(480.f-width)*.5f;
    canvas.text(canvas.user,label,left,y,size,selected?white:secondary);
    if(selected){
        appendMenuArrow(list,page,left-26,y+2,false);
        appendMenuArrow(list,page,left+width+10,y+2,true);
    }
}
struct SlotSummary {
    const char* area=nullptr;
    unsigned geo=0,masks=0;
};
using Slots=std::array<SlotSummary,hk3ds::saveSlotCount>;
void appendTitleMenu(DrawList& list,const MenuState& state,Canvas canvas,const Slots& slots){
    using Page=hk3ds::TitlePage;
    const auto selected=state.title.navigation.selected;
    if(state.title.page==Page::Options){
        appendPauseMenu(list,state.pause,0,canvas);return;
    }
    if(state.title.page==Page::Title){
        constexpr const char* labels[]={"Start Game","Options","Achievements","Extras","Quit Game"};
        for(unsigned i=0;i<5;++i)row(list,0,canvas,labels[i],150.f+i*20.f,i==selected);
    }else if(state.title.page==Page::Slots||state.title.page==Page::Clear){
        const bool clearing=state.title.page==Page::Clear;
        heading(list,0,canvas,"Select Profile",17);
        for(unsigned i=0;i<slots.size();++i){
            const auto& slot=slots[i];
            const float y=61.f+i*41.f;
            const bool focused=!clearing&&i==selected;
            const auto color=focused?white:secondary;
            if(focused){
                widget(list,0,profileSelection,65,y);
                appendMenuArrow(list,0,39,y+11,false);
                appendMenuArrow(list,0,325,y+11,true);
            }
            widget(list,0,profileFrame,65,y);
            if(clearing&&i==state.title.selectedSlot){
                canvas.text(canvas.user,"Clear Save?",102,y+12,Size::Heading,white);
                constexpr const char* choices[]={"Yes","No"};
                for(unsigned choice=0;choice<2;++choice){
                    const float left=227.f+63.f*choice;
                    const bool active=selected==1-choice;
                    canvas.text(canvas.user,choices[choice],left,y+12,Size::Heading,active?white:secondary);
                    if(active){
                        appendMenuArrow(list,0,left-20,y+11,false);
                        const float right=std::ceil(left+canvas.measure(canvas.user,choices[choice],Size::Heading));
                        appendMenuArrow(list,0,right+2,y+11,true);
                    }
                }
                continue;
            }
            char number[4];
            std::snprintf(number,sizeof(number),"%u.",i+1);
            canvas.text(canvas.user,number,76,y+12,Size::Heading,color);
            canvas.text(canvas.user,slot.area?slot.area:"New Game",104,y+12,Size::Heading,color);
            if(slot.area&&focused){
                char detail[64];
                std::snprintf(detail,sizeof(detail),"%u Geo    %u Masks",slot.geo,slot.masks);
                centered(canvas,detail,230,Size::Compact,secondary);
            }
        }
        row(list,0,canvas,"Back",250,!clearing&&selected==slots.size());
    }else{
        const bool achievements=state.title.page==Page::Achievements;
        heading(list,0,canvas,achievements?"Achievements":"Extras",28);
        centered(canvas,achievements?"Achievements are not available.":"Extras are not available.",
                 135,Size::Body);
    }
}
float moteUnit(unsigned seed){
    seed^=seed>>16;seed*=0x7feb352du;seed^=seed>>15;seed*=0x846ca68bu;seed^=seed>>16;
    return float(seed&0xffffu)/65535.f;
}
void appendTitleMotes(DrawList& list,unsigned page,float seconds){
    using namespace hk::menu_font;
    unsigned index=0;
    for(const auto& emitter:moteEmitters)for(unsigned i=0;i<emitter.count;++i,++index){
        const unsigned seed=(index+1)*2654435761u;
        const float life=emitter.lifeMin+(emitter.lifeMax-emitter.lifeMin)*moteUnit(seed+1);
        const float age=std::fmod(seconds+life*moteUnit(seed+2),life),phase=age/life;
        const float fade=std::min({1.f,phase/emitter.fadeIn,(1-phase)/(1-emitter.fadeOut)});
        const float vx=emitter.vxMin+(emitter.vxMax-emitter.vxMin)*moteUnit(seed+3);
        const float vz=emitter.vzMin+(emitter.vzMax-emitter.vzMin)*moteUnit(seed+4);
        const float x=std::fmod(moteUnit(seed+5)*480+vx*age*12+480,480.f);
        const float y=std::fmod(moteUnit(seed+6)*272+vz*age*.6f+272,272.f);
        const float size=(emitter.sizeMin+(emitter.sizeMax-emitter.sizeMin)*moteUnit(seed+7))*12;
        Quad quad;quad.page=page;quad.u1=moteWidth;quad.v1=moteHeight;
        const float left=x-size/2,top=y-size/2;
        quad.positions={{{left,top},{left+size,top},{left+size,top+size},{left,top+size}}};
        quad.rgba={emitter.r/255.f,emitter.g/255.f,emitter.b/255.f,fade*emitter.a/255.f};
        quad.order=list.nextOrder();list.quads.push_back(quad);
    }
}
}
void appendMenuDim(DrawList& list,float alpha){
    Quad q;q.blend=Blend::FadeBlack;q.rgba={0,0,0,alpha};
    q.positions={{{0,0},{480,0},{480,272},{0,272}}};
    q.order=list.nextOrder();list.quads.push_back(q);
}
void appendPauseMenu(DrawList& list,const hk3ds::PauseMenu& menu,unsigned page,Canvas canvas){
    using Screen=hk3ds::PauseMenu::Screen;
    const auto screen=menu.screen;
    const bool paused=screen==Screen::Pause,controls=screen==Screen::Controls;
    // The original pause screen has no area title above the menu; submenus keep their headings.
    if(!paused){
        const float headingY=menu.atMainMenu()?30.f:25.f;
        const char* label=controls?"Controller":
            screen==Screen::Game?"Game Options":screen==Screen::Audio?"Audio":"Options";
        heading(list,page,canvas,label,headingY);
    }
    if(paused){
        widget(list,page,pauseTop,187,76);
        widget(list,page,pauseBottom,202,197);
    }
    if(controls){
        constexpr const char* labels[]={"Analog Nub / D-pad: Move","Cross: Jump    Square: Attack",
            "Circle: Cast / Hold Focus    Triangle: Dream Nail","R: Dash    L: Super Dash",
            "Up / Down: Look / Interact    Start: Pause"};
        for(unsigned i=0;i<5;++i)centered(canvas,labels[i],81.f+i*16.f,Size::Compact);
        row(list,page,canvas,"Back",231.f,true);
    }else if(screen==Screen::Audio){
        for(unsigned i=0;i<3;++i)
            audioSlider(list,page,canvas,menu.label(i),76.f+23.f*i,menu.pspAudioVolume(i),menu.rows.selected==i);
        row(list,page,canvas,menu.label(3),205.f,menu.rows.selected==3);
        row(list,page,canvas,menu.label(4),231.f,menu.rows.selected==4);
    }else if(screen==Screen::Game){
        using Menu=hk3ds::PauseMenu;
        for(unsigned i=0;i<Menu::ResetGame;++i){
            const float y=72.f+23.f*i;
            const bool selected=menu.rows.selected==i;
            const auto color=selected?white:secondary;
            const char* value=menu.pspGameValue(i);
            canvas.text(canvas.user,menu.label(i),133,y,Size::Heading,color);
            canvas.text(canvas.user,value,347.f-canvas.measure(canvas.user,value,Size::Compact),y,Size::Compact,color);
            if(selected){
                appendMenuArrow(list,page,108,y+2,false);
                appendMenuArrow(list,page,357,y+2,true);
            }
        }
        row(list,page,canvas,menu.label(Menu::ResetGame),205.f,menu.rows.selected==Menu::ResetGame);
        row(list,page,canvas,menu.label(Menu::GameBack),231.f,menu.rows.selected==Menu::GameBack);
    }else{
        const float first=paused?107.f:76.f;
        const float gap=paused?30.f:24.f;
        for(unsigned i=0;i<menu.count();++i){
            const float y=!paused&&i+1==menu.count()?231.f:first+i*gap;
            row(list,page,canvas,menu.label(i),y,i==menu.rows.selected);
        }
    }
    if(menu.saveFailed)centered(canvas,"Save failed - try again",238,Size::Compact,secondary);
}
bool presentTitle(Platform& platform,MenuRuntime& menus,unsigned frames){
    Slots slots{};
    std::array<bool,hk3ds::saveSlotCount> occupied{};
    for(unsigned slot=0;slot<occupied.size();++slot){
        const auto path=menuSlotPath(platform.savePath(),slot);
        auto saved=std::make_unique<hk3ds::TutorialSave>();
        occupied[slot]=!path.empty()&&saved->load(path.c_str());
        if(occupied[slot])slots[slot]={hk3ds::saveAreaName(saved->scene),saved->geo,
                                     5u+unsigned(saved->collectibles.maskShards/4)};
    }
    loadGameText(platform);  // Checks the data format before any other data file.
    std::vector<Texture> textures;
    textures.push_back(readTexture(platform.path("menu-fill.hpt")));
    textures.push_back(readTexture(platform.path("title-background.hpt")));
    textures.push_back(readTexture(platform.path("title-logo.hpt")));
    textures.push_back(readTexture(platform.path("title-mote.hpt")));
    const auto fonts=readFonts(platform.path("fonts.bin"));
    const auto fontPage=unsigned(textures.size());
    for(unsigned i=0;i<fonts.pageCount;++i){
        auto texture=readTexture(platform.path("font-page-"+std::to_string(i)+".hpt"),true);
        if(texture.width!=256||texture.height!=256||texture.bits!=4)
            throw std::runtime_error("PFT1 page contract failed");
        textures.push_back(std::move(texture));
    }
    DrawList list;list.textures=&textures;
    TextCanvas text(list,fonts,fontPage);
#ifdef __PSP__
    for(unsigned i=fontPage;i<textures.size();++i)
        std::vector<unsigned char>().swap(textures[i].indices);
#endif
    const unsigned firstTitleFrame=menus.presented;
    while(menus.available(frames)){
        const auto previous=menus.state;
        const auto action=menus.state.update(menus.pollUi(platform),occupied);
        if(action==MenuState::Action::SettingsChanged)menus.saveSettings();
        if(action==MenuState::Action::ExitGame){
            platform.releaseRoomAssets(0);return false;
        }
        if(action==MenuState::Action::StartGame){
            menus.guard.block(platform.poll().held);
        }
        if(action==MenuState::Action::ClearSlot){
            const unsigned slot=menus.state.title.selectedSlot;
            const auto path=menuSlotPath(platform.savePath(),slot);
            if(!path.empty()&&!hk3ds::TutorialSave::clear(path.c_str()))
                throw std::runtime_error("Cannot clear save slot");
            occupied[slot]=false;
            slots[slot]={};
        }
        list.quads.clear();list.info.frame=menus.presented;
        list.info.controlSteps=menus.controlSteps;list.info.scene="Title";
        const auto image=[&](unsigned page,float x,float y,float width,float height,float scale){
            Quad q;q.page=page;q.u1=width;q.v1=height;
            q.positions={{{x,y},{x+width*scale,y},{x+width*scale,y+height*scale},{x,y+height*scale}}};
            q.order=list.nextOrder();list.quads.push_back(q);
        };
        image(1,0,0,480,272,1);
        appendTitleMotes(list,3,float(menus.presented-firstTitleFrame)/30.f);
        if(menus.state.title.page==hk3ds::TitlePage::Title)image(2,90,24,300,123,1);
        appendTitleMenu(list,action==MenuState::Action::StartGame?previous:menus.state,text.canvas(),slots);
        platform.beginFrame();platform.submit(list);platform.endFrame();++menus.presented;
        if(action==MenuState::Action::StartGame){platform.releaseRoomAssets(0);return true;}
    }
    platform.releaseRoomAssets(0);return false;
}
}
