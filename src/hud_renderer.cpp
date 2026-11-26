#include "text_pack.hpp"
#include "route_tolls.hpp"
#include <memory>
#include "text_capture.hpp"
#include "menu.hpp"
#include "inventory_renderer.hpp"
#include "simple_map.hpp"
#include "hud_renderer.hpp"
#include "ui_window.hpp"
#include "load_diagnostics.hpp"
#include "sly_shop.hpp"
#include "cartographer.hpp"
#include "shop_menu.hpp"
#include "ability_pickup.hpp"
#include "pause_menu.hpp"
#include "fury_chest.hpp"
#include "resting.hpp"
#include "conditional_events.hpp"
#include "renderer.hpp"
#include "hud_data.hpp"
#include "hud_glyphs.hpp"
#include "inventory_layout.hpp"
#include "hud_text_layout.hpp"
#include "tutorial_prompt_glyphs.hpp"
#include "tutorial_prompt_layout.hpp"
#include "lore_tablet_layout.hpp"
#include "top_screen_ui_layout.hpp"
#include "render_memory.hpp"
#include "sprite_layout.hpp"
#include "mushroom_dialogue_data.hpp"
#include "world_quests.hpp"
#include "world_quest_dialogue_layout.hpp"
#include "cartographer.hpp"
#include "effect_passes.hpp"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <cctype>

extern const unsigned char effects_shbin[];
extern const unsigned int effects_shbin_size;

namespace hk3ds {
struct SlyShopArt {
    AnimationBank bank;
    C2D_SpriteSheet page=nullptr;
    ~SlyShopArt(){if(page)C2D_SpriteSheetFree(page);}
};

void HudRenderer::drawResting(const Resting& rest,unsigned geo,bool detail) const {
    C2D_ViewReset();
    const auto c=uiCanvas();
    const bool toll=rest.stagPhase==StagPhase::Toll||rest.stagPhase==StagPhase::Poor;
    if(!rest.travelMenu&&!rest.charmMenu&&!toll){
        // Original Bench Control FSM: no prompt while resting; the near-bench "Rest" is the top-screen Arrow Prompt.
        return;
    }
    if(detail){
        const auto bit=1u<<rest.selection;
        const char* name=rest.travelMenu?stagStations[rest.selection].label:rest.charmLabel();
        const char* status=rest.travelMenu?((rest.saved.stations&rest.availableStations&bit)?"Station unlocked":"Station locked"):
            !(rest.saved.ownedCharms&bit)?"Not owned":rest.saved.equippedCharms&bit?"Equipped":"Unequipped";
        ui::detail(c,name,status,rest.travelMenu?"Travel through Hallownest with the Last Stag.":"Rest at a bench to change equipped charms.",{20,80,360,152});
        return;
    }
    if(toll){
        ui::bottomBackground(c);
        ui::ChoiceDialog dialog{hk3ds::text::resolve("@Prompts/TOLLBOOTH_STAG#2"),rest.station.price,
                                rest.yes,geo>=rest.station.price&&rest.stagPhase!=StagPhase::Poor};
        ui::choiceDialog(dialogCanvas(),dialog);
        return;
    }
    if(rest.travelMenu){
        std::array<ui::StagStation,stagStationCount> stations{};
        for(unsigned i=0;i<stagStationCount;++i){
            stations[i]={stagStations[i].label,
                (rest.saved.stations&rest.availableStations&(1u<<i))!=0,
                rest.station.present&&rest.station.station==i,0,0};
        }
        ui::bottomBackground(c);
        ui::stagStationList(dialogCanvas(),{stations.data(),stagStationCount,rest.selection});
        return;
    }
    ui::window(c,"Charms");
    ui::label(c,rest.charmLabel(),36,80);
    const auto bit=1u<<rest.selection;
    ui::label(c,!(rest.saved.ownedCharms&bit)?"Not owned":rest.saved.equippedCharms&bit?"Equipped":"Unequipped",36,116);
    ui::label(c,"Three notches available",36,152);
    const ui::Action actions[]={{"Up/Down","Select"},{"X","OK"},{"B","Back"}};
    ui::promptBar(c,actions,3);
}
void HudRenderer::drawResting(const Resting& rest,float stationX,float stationY) const {
    const auto phase=rest.stagPhase;
    const bool toll=phase==StagPhase::Toll||phase==StagPhase::Poor;
    const bool dialogue=phase==StagPhase::Dialogue;
    if(!rest.travelMenu&&!rest.charmMenu&&!toll&&!dialogue){
        // User 2026-11-12: Inspect and Listen at stations are only the top-screen Arrow Prompt (TutorialSession::gameplayPrompt);
        // the port "UP: Inspect" / "UP: Listen" text and their plates are removed. The bell keeps one text prompt, without a plate.
        const char* prompt=rest.sitting?"ATTACK: Charms    JUMP: Stand":rest.bellPrompt?"X: Ring bell":nullptr;
        if(prompt){
            const bool stationPrompt=!rest.sitting&&rest.bellPrompt;
            if(stationPrompt){
                // Keep the text above its station interaction, including when that interaction is near a screen edge.
                const float x=std::clamp(stationX,95.f,305.f);
                const float y=std::clamp(stationY,46.f,210.f);
                text(prompt,x,y+2,1,hudtext::stationPromptScale);
            }else{
                // User 2026-11-12 ("2"): the bench prompt sits on the scene without a dark plate.
                text(prompt,200,224,1,hudtext::restingPromptScale);
            }
        }
        return;
    }
    if(toll)return;
    if(rest.travelMenu){
        return;
    }
    C2D_DrawRectSolid(18,24,0,364,192,C2D_Color32(5,7,12,245));
    if(toll||dialogue||rest.travelMenu){
        // Reuse resident original artwork for the compact 3DS station panel.
        sprite(frame(loreFleurTop_),200,24,.8f);
        sprite(frame(loreFleurBottom_),200,224,.8f);
    }
    text(rest.travelMenu?"STAG STATIONS":rest.charmMenu?"CHARMS":toll?"STAGWAY":"THE LAST STAG",200,49,1,hudtext::stationMenuScale);
    if(toll){
        char line[80];std::snprintf(line,sizeof(line),"Pay %u Geo to open this Stag Station?",rest.station.price);
        text(phase==StagPhase::Poor?"Not enough Geo.":line,200,100,1,.75f);
        if(phase==StagPhase::Toll)text(rest.yes?"> Yes                 No":"  Yes               > No",200,146,1,.9f);
    }else if(dialogue){
        const char* start=rest.stationText();unsigned row=0;
        while(*start&&row<4){const char* end=std::strchr(start,'\n');if(!end)end=start+std::strlen(start);char line[128]{};std::snprintf(line,sizeof(line),"%.*s",int(end-start),start);text(line,200,80+row*23,1,.75f);++row;start=*end?end+1:end;}
    }else if(rest.travelMenu){
        unsigned row=0;
        for(unsigned i=0;i<stagStationCount;++i)if((i==rest.station.station||(rest.destinations()&(1u<<i)))&&i+3>=rest.selection&&i<=rest.selection){
            const bool here=i==rest.station.station;
            char line[80];std::snprintf(line,sizeof(line),"%s %s%s",!here&&rest.selection==i?">":" ",stagStations[i].label,here?" (here)":"");
            text(line,200,82+row++*27,1,hudtext::stationMenuScale);
        }
        if(!rest.destinations())text("No other Stag Stations opened.",200,110,1,.8f);
    }else{
        text(rest.charmLabel(),200,99,1,1);
        const auto bit=1u<<rest.selection;
        text(!(rest.saved.ownedCharms&bit)?"Not owned":rest.saved.equippedCharms&bit?"Equipped":"Unequipped",200,129,1,.8f);
        text("Three notches available",200,156,.7f,.75f);
    }
    text(dialogue?"ATTACK: Continue    JUMP: Back":"UP/DOWN: Select  ATTACK: OK  JUMP: Back",200,201,1,.7f);
}
void HudRenderer::drawDoorPrompt(const char* label,float x,float y,float alpha) const
{
    if(!label||!*label||alpha<=0)return;
    char uppercase[32]{};
    for(unsigned i=0;i+1<sizeof(uppercase)&&label[i];++i)
        uppercase[i]=static_cast<char>(std::toupper(static_cast<unsigned char>(label[i])));
    text(uppercase,x,y+4.5f,alpha,.8625f);
}

void HudRenderer::drawCharms(const CharmInventory& charms,const CharmsPage& page,bool canEquip,unsigned geo,const AnimationBank* items,const HeroRenderer* renderer,bool embedded,bool detail) const
{
    (void)geo; // Geo is drawn by the shared HUD pass; charm pages only need its layout slot.
    C2D_ViewReset();C3D_DepthTest(false,GPU_ALWAYS,GPU_WRITE_COLOR);
    const auto icon=[&](unsigned id,float x,float y,float size,float alpha){
        char name[24];std::snprintf(name,sizeof(name),"Charm%u",id);
        if(inventoryArt_&&inventoryArt_->drawIconFit(name,{x-size/2,y-size/2,size,size},alpha))return;
        if(id==6&&items&&renderer){
            AnimationPlayer player;player.play(items->find("Fury Icon"));
            if(const auto* source=player.current(*items)){
                const auto& f=*source;const float scale=size/std::max(f.right-f.left,f.top-f.bottom);
                renderer->drawAnimation(player,*items,x-(f.left+f.right)*scale/2,y+(f.bottom+f.top)*scale/2,1,scale,scale,alpha);return;
            }
        }
    };
    if(!page.open){
        float x=143;
        for(const auto& d:charmDefinitions)if(charms.wears(d.id)){icon(d.id,x,17,12,1);x+=15;}
        return;
    }
    if(detail){
        const auto& d=charmDefinitions[page.selected];
        char value[96];std::snprintf(value,sizeof(value),"Cost %u   Notches %u/%u",d.cost,charms.used(),unsigned(charms.notches));
        const auto c=uiCanvas();
        ui::panel(c,{0,0,400,240});
        icon(d.id,60,54,64,1);
        ui::wrappedText(c,d.name,inventorylayout::charmNameBounds,ui::Size::Heading);
        ui::wrappedText(c,value,{104,56,272,20});
        if(!charms.has(d.id))ui::wrappedText(c,"Not collected",{104,78,272,20});
        ui::wrappedText(c,hk3ds::text::resolve(d.description),inventorylayout::charmDescriptionBounds);
        return;
    }
    if(!embedded){
        C2D_DrawRectSolid(0,0,0,400,240,C2D_Color32(5,8,17,250));
        text("CHARMS",200,24,1,ui::scale(ui::Size::Heading));
        C2D_DrawLine(30,33,C2D_Color32(145,151,169,255),370,33,C2D_Color32(145,151,169,255),1,0);
    }
    if(embedded){
        ui::wrappedText(uiCanvas(),"Equipped",{20,44,92,20});
        ui::wrappedText(uiCanvas(),charms.overcharmed()?"OVERCHARMED":"Notches",{20,76,92,20});
    }
    else text(charms.overcharmed()?"OVERCHARMED":"CHARM NOTCHES",100,49,1,ui::scale(ui::Size::Body));
    for(unsigned n=0;n<std::max<unsigned>(charms.notches,charms.used());++n){
        const auto color=n>=charms.notches?C2D_Color32(185,83,223,255):n<charms.used()?C2D_Color32(232,231,220,255):C2D_Color32(53,61,79,255);
        C2D_DrawEllipseSolid(inventorylayout::notchX(n,embedded),inventorylayout::notchY(embedded),0,10,10,color);
    }
    for(unsigned i=0;i<charmDefinitions.size();++i){
        const auto& d=charmDefinitions[i];const float x=inventorylayout::charmX(i,embedded),y=inventorylayout::charmY(i,embedded);
        C2D_DrawEllipseSolid(x-10,y-10,0,20,20,C2D_Color32(45,49,57,255));
        C2D_DrawEllipseSolid(x-7,y-7,0,14,14,C2D_Color32(5,8,13,255));
        if(charms.has(d.id))icon(d.id,x,y,26,1);
        if(i==page.selected&&inventoryArt_)inventoryArt_->drawSelection({x-19,y-19,38,38});
        if(charms.wears(d.id))C2D_DrawEllipseSolid(x-3,y+11,0,6,6,C2D_Color32(226,218,170,255));
    }
    const auto& d=charmDefinitions[page.selected];
    ui::wrappedText(uiCanvas(),d.name,{20,170,280,24},ui::Size::Heading);
    const ui::Action actions[]={{"A",page.overcharmAttempts?"Confirm":charms.wears(d.id)?"Unequip":"Equip"},{"B","Back"},{"SELECT","Page"}};
    if(canEquip&&charms.has(d.id))ui::promptBar(uiCanvas(),actions,3);
    else ui::promptBar(uiCanvas(),actions+1,2);
    // Equipped art occupies the strip before the grid, beside the notch dots.
    float equippedX=126;
    for(const auto& equipped:charmDefinitions)if(charms.wears(equipped.id)){
        icon(equipped.id,equippedX,54,18,1);equippedX+=18;
    }
    if(equippedX==126){
        C2D_DrawEllipseSolid(120,48,0,12,12,C2D_Color32(45,49,57,255));
        C2D_DrawEllipseSolid(123,51,0,6,6,C2D_Color32(5,8,13,255));
    }

}
bool HudRenderer::load(const char* directory)
{
    release();
    char path[256];
    const auto fail=[&](const char* stage){loadDiagnostics.error(stage,path);release();return false;};
    std::snprintf(path,sizeof(path),"%s/hud-visual.bin",directory);
    if(!bank_.load(path)||bank_.pageCount!=pages_.size())return fail("hud_metadata_failed");
    healthEmpty_=bank_.find("Health Empty");healthIdle_=bank_.find("Health Idle");
    healthBreak_=bank_.find("Health Break");healthRefill_=bank_.find("Health Refill");
    blueIdle_=bank_.find("Blue Idle");blueBreak_=bank_.find("Blue Break");blueAppear_=bank_.find("Blue Appear");
    coinIdle_=bank_.find("Coin Idle");coinGet_=bank_.find("Coin Get Loop");soulShape_=bank_.find("Soul Mask");
    coinAppear_=bank_.find("Coin Appear");coinGetOnce_=bank_.find("Coin Get");
    soulEyes_=bank_.find("Soul Eyes");frame_=bank_.find("HUD Frame Idle");
    liquidIdle_=bank_.find("Liquid Idle");liquidShrink_=bank_.find("Liquid Shrink");
    soulLimiter_=bank_.find("Soul Limiter Idle");
    limiterBurst_=bank_.find("Soul Limiter Burst");limiterFragment_=bank_.find("Soul Limiter Fragment");
    crackedFrame_=bank_.find("HUD Frame Cracked");
    soulFlash_=bank_.find("Soul Gain Flash");soulGlow_=bank_.find("Soul Full Glow");
    soulRim_=bank_.find("Soul Full Rim");soulBurst_=bank_.find("Soul Burst");
    refillParticle_=bank_.find("Health Refill Particle");
    plus_=bank_.find("Glyph Plus");minus_=bank_.find("Glyph Minus");
    if(!healthEmpty_||!healthIdle_||!healthBreak_||!healthRefill_||!coinIdle_||
       !blueIdle_||!blueBreak_||!blueAppear_||!coinGet_||!coinAppear_||!coinGetOnce_||!plus_||!minus_||
       !soulShape_||!soulEyes_||!liquidIdle_||!liquidShrink_||!frame_||!crackedFrame_||
       !soulFlash_||!soulGlow_||!soulRim_||!soulBurst_||!refillParticle_||!limiterBurst_||!limiterFragment_){release();return false;}
    for(unsigned i=0;i<digits_.size();++i){
        char name[16];std::snprintf(name,sizeof(name),"Digit%u",i);
        digits_[i]=bank_.find(name);
        if(!digits_[i]){release();return false;}
    }
    for(unsigned size=0;size<promptGlyphs_.size();++size){
        for(unsigned i=0;i<promptGlyphs_[size].size();++i){
            char name[32];std::snprintf(name,sizeof(name),"LCD Glyph %u %u",size,i+32);
            promptGlyphs_[size][i]=bank_.find(name);
            if(!promptGlyphs_[size][i]){release();return false;}
        }
    }
    promptBackboard_=bank_.find("Prompt Backboard");focusPanel_=bank_.find("Focus Panel");
    promptButton_=bank_.find("Prompt Button");
    for(unsigned i=0;i<creditLabels_.size();++i){
        char name[32];std::snprintf(name,sizeof(name),"Credit %u",creditsdata::texts[i].owner);
        creditLabels_[i]=bank_.find(name);
        if(!creditLabels_[i]){release();return false;}
    }
    for(unsigned i=0;i<areaTitleParts_.size();++i){
        char name[32];std::snprintf(name,sizeof(name),"Area Title %u",i);
        areaTitleParts_[i]=bank_.find(name);
    }
    for(unsigned i=0;i<areaTitleOrnaments_.size();++i){
        char name[32];std::snprintf(name,sizeof(name),"Area Title Fleur %u",i);
        areaTitleOrnaments_[i]=bank_.find(name);
    }
    loreBackboard_=bank_.find("Lore Backboard");
    loreArrow_=bank_.find("Lore Arrow");loreStop_=bank_.find("Lore Stop");
    loreFleurTop_=bank_.find("Lore Fleur Top");loreFleurBottom_=bank_.find("Lore Fleur Bottom");
    pickupFleur_=bank_.find("Menu Fleur Top 70");
    for(unsigned i=0;i<loreGlyphs_.size();++i){
        char name[32];std::snprintf(name,sizeof(name),"Lore Glyph %u",i+32);
        loreGlyphs_[i]=bank_.find(name);
    }
    if(!promptBackboard_||!focusPanel_||!promptButton_){release();return false;}
    for(unsigned index=0;index<pages_.size();++index){
        std::snprintf(path,sizeof(path),"%s/hud-page-%u.t3x",directory,index);
        pages_[index]=C2D_SpriteSheetLoad(path);
        if(!pages_[index])return fail("hud_page_import_failed");
        const auto image=C2D_SpriteSheetGetImage(pages_[index],0);
        if(!image.tex||!image.subtex)return fail("hud_page_layout_failed");
        C3D_TexSetFilter(image.tex,GPU_NEAREST,GPU_NEAREST);
    }
    // Resident RGB8 background and RGBA4 font; no title-only logo.
    if(!menuArt_.load(false)||menuArt_.textureBytes()>hudMenuTextureBudgetBytes||
       textureBytes()>hudTextureBudgetBytes||linearSpaceFree()<hudLinearReserveBytes){release();return false;}
    for(const auto& s:bank_.sprites){
        if(s.page>=pages_.size()){release();return false;}
        const auto image=C2D_SpriteSheetGetImage(pages_[s.page],0);
        if(s.x+s.width>image.tex->width||s.y+s.height>image.tex->height){release();return false;}
    }
    liquidShader_=DVLB_ParseFile(reinterpret_cast<u32*>(const_cast<unsigned char*>(effects_shbin)),effects_shbin_size);
    if(!liquidShader_){release();return false;}
    liquidProgramReady_=R_SUCCEEDED(shaderProgramInit(&liquidProgram_));
    if(!liquidProgramReady_||R_FAILED(shaderProgramSetVsh(&liquidProgram_,&liquidShader_->DVLE[0]))){release();return false;}
    // Shop icons are optional art: a missing or invalid icon package must never
    // take the gameplay HUD down with it (the shop then draws without icons).
    const auto loadShopArt=[&]{
        auto art=std::make_unique<SlyShopArt>();
        char shopPath[256];
        std::snprintf(shopPath,sizeof(shopPath),"%s/sly-shop-icons.bin",directory);
        if(!art->bank.load(shopPath)||art->bank.pageCount!=1)return false;
        std::snprintf(shopPath,sizeof(shopPath),"%s/sly-shop-icons.t3x",directory);
        art->page=C2D_SpriteSheetLoad(shopPath);
        if(!art->page)return false;
        const auto shopTexture=C2D_SpriteSheetGetImage(art->page,0);
        if(!shopTexture.tex||shopTexture.tex->size>slyShopTextureBudgetBytes)return false;
        for(const auto& icon:art->bank.sprites)
            if(icon.page||icon.x+icon.width>shopTexture.tex->width||icon.y+icon.height>shopTexture.tex->height)return false;
        for(const auto& item:slyItems)if(!art->bank.find(item.flag))return false;
        shopArt_=art.release();
        return true;
    };
    loadShopArt();
    if(textureBytes()>hudTextureBudgetBytes||linearSpaceFree()<hudLinearReserveBytes){release();return false;}
    // HUD sprites share the world's top-screen target; no extra transfer.
    seconds_=menuSeconds_=0;draws_=0;dirty_=true;
    return true;
}
void HudRenderer::release()
{
    delete shopArt_;shopArt_=nullptr;
    menuArt_.release();
    if(liquidProgramReady_)shaderProgramFree(&liquidProgram_);
    if(liquidShader_)DVLB_Free(liquidShader_);
    liquidShader_=nullptr;liquidProgramReady_=false;
    for(auto page:pages_)if(page)C2D_SpriteSheetFree(page);
    pages_={};
    bank_={};digits_={};seconds_=menuSeconds_=0;dirty_=true;
    healthEmpty_=healthIdle_=healthBreak_=healthRefill_=nullptr;
    blueIdle_=blueBreak_=blueAppear_=nullptr;
    coinIdle_=coinGet_=soulShape_=soulEyes_=frame_=nullptr;
    soulLimiter_=crackedFrame_=nullptr;
    limiterBurst_=limiterFragment_=nullptr;
    coinAppear_=coinGetOnce_=nullptr;
    liquidIdle_=liquidShrink_=nullptr;lastLiquidFrame_=nullptr;
    soulFlash_=soulGlow_=soulRim_=soulBurst_=refillParticle_=nullptr;
    plus_=minus_=nullptr;
    promptGlyphs_={};promptBackboard_=focusPanel_=promptButton_=nullptr;prompts_=nullptr;promptRevision_=~0u;
    areaTitleParts_={};
    areaTitleOrnaments_={};
    creditLabels_={};
    loreBackboard_=nullptr;inspection_=nullptr;inspectionRevision_=~0u;
    loreArrow_=loreStop_=nullptr;
    loreFleurTop_=loreFleurBottom_=pickupFleur_=nullptr;loreGlyphs_={};
}
std::size_t HudRenderer::textureBytes() const
{
    std::size_t bytes=0;
    if(shopArt_&&shopArt_->page){const auto image=C2D_SpriteSheetGetImage(shopArt_->page,0);if(image.tex)bytes+=image.tex->size;}
    for(auto page:pages_)if(page){
        const auto image=C2D_SpriteSheetGetImage(page,0);
        if(image.tex)bytes+=image.tex->size;
    }
    return bytes;
}
const SpriteFrame* HudRenderer::frame(const Clip* clip,float seconds,unsigned index) const
{
    AnimationPlayer player;player.play(clip);
    if(index==~0u)player.advance(seconds);
    else if(clip)player.frame=std::min<std::uint32_t>(index,clip->count-1);
    return player.current(bank_);
}
void HudRenderer::sprite(const SpriteFrame* s,float x,float y,float scale,const C2D_ImageTint* tint,float verticalScale,bool snap,float rotation) const
{
    if(!s)return;
    const auto page=C2D_SpriteSheetGetImage(pages_[s->page],0);
    Tex3DS_SubTexture sub{};
    sub.width=static_cast<u16>(s->width);sub.height=static_cast<u16>(s->height);
    sub.left=float(s->x)/page.tex->width;sub.right=float(s->x+s->width)/page.tex->width;
    sub.top=1-float(s->y)/page.tex->height;sub.bottom=1-float(s->y+s->height)/page.tex->height;
    const auto layout=spriteLayout(*s,x,y,huddata::pixelsPerUnit,scale,scale*verticalScale);
    if(rotation==0){
        C3D_Mtx view;C2D_ViewSave(&view);
        const float px=snap?topui::snapPixel(layout.x,view.r[0].x,view.r[0].w):layout.x;
        const float py=snap?topui::snapPixel(layout.y,view.r[1].y,view.r[1].w):layout.y;
        textcapture::glyph(px,py,s->width*layout.scaleX,s->height*layout.scaleY);
        C2D_DrawImageAt({page.tex,&sub},px,py,0,tint,layout.scaleX,layout.scaleY);
    }
    else {
        const auto rotated=rotatedSpriteLayout(*s,x,y,huddata::pixelsPerUnit,scale,scale*verticalScale,rotation);
        C2D_DrawImageAtRotated({page.tex,&sub},snap?std::round(rotated.x):rotated.x,snap?std::round(rotated.y):rotated.y,0,-rotation,tint,rotated.scaleX,rotated.scaleY);
    }
}
void HudRenderer::number(std::uint32_t value,float x,float y,float alpha) const
{
    C2D_ImageTint tint{};C2D_AlphaImageTint(&tint,alpha);
    char text[11];std::snprintf(text,sizeof(text),"%lu",static_cast<unsigned long>(value));
    textcapture::String capture(text);
    for(const char* p=text;*p;++p){
        sprite(frame(digits_[*p-'0']),x,y,1,&tint,1,true);x+=huddata::geoAdvance[*p-'0'];
    }
}
void HudRenderer::soulFeedback(const HudPresentation& hud) const
{
    C2D_ImageTint tint{};
    if(hud.soulFull()){
        sprite(frame(soulGlow_),huddata::frameX,huddata::frameY);
        C2D_AlphaImageTint(&tint,hud.soulRimAlpha());
        sprite(frame(soulRim_),huddata::frameX,huddata::frameY,1,&tint);
    }
    if(hud.soulBurstSeconds()<huddata::soulBurstSeconds)
        sprite(frame(soulBurst_,hud.soulBurstSeconds()),huddata::frameX,huddata::frameY);
    if(hud.soulFlashAlpha()>0){
        C2D_AlphaImageTint(&tint,hud.soulFlashAlpha());
        sprite(frame(soulFlash_),huddata::frameX,huddata::frameY,1,&tint);
    }
}
void HudRenderer::limiterFeedback(const HudPresentation& hud) const
{
    if(hud.limiterBurstSeconds()<huddata::limiterBurstSeconds)
        sprite(frame(limiterBurst_,hud.limiterBurstSeconds()),huddata::frameX,huddata::frameY);
    for(const auto& p:hud.limiterFragments())if(p.visible()){
        C2D_ImageTint tint{};C2D_AlphaImageTint(&tint,p.alpha());
        sprite(frame(limiterFragment_,0,p.variant),huddata::frameX+p.x*huddata::pixelsPerUnit,
            huddata::frameY-p.y*huddata::pixelsPerUnit,p.currentSize(),&tint,1,false,p.rotation);
    }
}
void HudRenderer::refillFeedback(const HudPresentation& hud) const
{
    assert(hud.refillDrops()==0);
    bool additive=false;
    for(const auto& p:hud.refillParticles())if(p.visible()){
        if(!additive){
            C2D_Flush();
            C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_SRC_ALPHA,GPU_ONE,GPU_ONE,GPU_ONE_MINUS_SRC_ALPHA);
            additive=true;
        }
        const float x=p.emitter==2?huddata::frameX:huddata::firstMaskX+p.mask*huddata::maskStep;
        const float y=p.emitter==2?huddata::frameY:huddata::maskY;
        sprite(frame(refillParticle_),x+p.x*huddata::pixelsPerUnit,y-p.positionY()*huddata::pixelsPerUnit,p.currentSize());
    }
    if(additive){
        C2D_Flush();
        C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_SRC_ALPHA,GPU_ONE_MINUS_SRC_ALPHA,GPU_ONE,GPU_ONE_MINUS_SRC_ALPHA);
    }
}
void HudRenderer::liquid(C3D_RenderTarget* target,const HudPresentation& hud)
{
    if(hud.displayedSoul()<=1)return; // Source MP <= 1 disables the liquid renderer.
    const auto* s=frame(hud.liquidAnimation()==HudLiquidAnimation::Shrink?liquidShrink_:liquidIdle_,hud.liquidSeconds());
    const auto* shape=frame(soulShape_);
    const auto layout=spriteLayout(*s,huddata::soulX+huddata::liquidX*huddata::pixelsPerUnit,
        huddata::soulY-(huddata::liquidBottomY+huddata::liquidYPerSoul*hud.displayedSoul())*huddata::pixelsPerUnit,
        huddata::pixelsPerUnit,huddata::liquidScale,huddata::liquidScale);
    auto mask=spriteLayout(*shape,huddata::soulX,huddata::soulY,huddata::pixelsPerUnit,1,1);
    C3D_Mtx view,projection,transform;
    C2D_ViewSave(&view);
    // The vessel silhouette stays on the same LCD grid as its artwork; the
    // liquid's independent UVs retain their authored continuous movement.
    mask.x=topui::snapPixel(mask.x,view.r[0].x,view.r[0].w);
    mask.y=topui::snapPixel(mask.y,view.r[1].y,view.r[1].w);
    const float x=std::max(layout.x,mask.x),y=std::max(layout.y,mask.y);
    const float right=std::min(layout.x+s->width*layout.scaleX,mask.x+shape->width*mask.scaleX);
    const float bottom=std::min(layout.y+s->height*layout.scaleY,mask.y+shape->height*mask.scaleY);
    if(right<=x||bottom<=y)return;
    const auto page=C2D_SpriteSheetGetImage(pages_[s->page],0);
    const auto maskPage=C2D_SpriteSheetGetImage(pages_[shape->page],0);
    const bool tilted=target->linked;
    const float width=tilted?target->frameBuf.height:target->frameBuf.width;
    const float height=tilted?target->frameBuf.width:target->frameBuf.height;
    if(tilted)Mtx_OrthoTilt(&projection,0,width,height,0,0,1,true);
    else Mtx_Ortho(&projection,0,width,height,0,0,1,true);
    Mtx_Multiply(&transform,&projection,&view);
    C2D_Flush();C3D_BindProgram(&liquidProgram_);
    auto* attrs=C3D_GetAttrInfo();AttrInfo_Init(attrs);
    for(int i=0;i<4;++i)AttrInfo_AddLoader(attrs,i,GPU_FLOAT,4);
    effects::alphaMaskedSprite(*page.tex,*maskPage.tex,hud.liquidTone());
    static constexpr unsigned corners[6][2]={{0,0},{1,0},{0,1},{1,0},{1,1},{0,1}};
    C3D_ImmDrawBegin(GPU_TRIANGLES);
    for(const auto& corner:corners){
        const float px=corner[0]?right:x,py=corner[1]?bottom:y;
        const auto position=Mtx_MultiplyFVec4(&transform,FVec4_New(px,py,0,1));
        C3D_ImmSendAttrib(position.x,position.y,position.z,position.w);
        C3D_ImmSendAttrib((s->x+(px-layout.x)/layout.scaleX)/page.tex->width,
                         1-(s->y+(py-layout.y)/layout.scaleY)/page.tex->height,0,1);
        C3D_ImmSendAttrib((shape->x+(px-mask.x)/mask.scaleX)/maskPage.tex->width,
                         1-(shape->y+(py-mask.y)/mask.scaleY)/maskPage.tex->height,0,1);
        C3D_ImmSendAttrib(0,0,0,1);
    }
    C3D_ImmDrawEnd();
    // Unit 1 keeps the mask page (a resident sheet): citro2d's env samples only unit 0, and
    // C3D_TexBind(1,nullptr) dereferences null in citro3d 1.7.1.
    C2D_Prepare();C2D_ViewRestore(&view);
    C3D_DepthTest(false,GPU_ALWAYS,GPU_WRITE_COLOR);
    C3D_AlphaTest(false,GPU_ALWAYS,0);effects::spriteBlend();
}
void HudRenderer::drawBossTitle(float alpha) const
{
    if(alpha>0)menuArt_.text("False Knight",hudtext::bossTitleX,hudtext::bossTitleBaseline-12,
                           hudtext::bossTitleScale,true,C2D_Color32f(1,1,1,alpha));
}
void HudRenderer::drawHornetTitle(float alpha) const
{
    if(alpha>0)menuArt_.text("Hornet",hudtext::bossTitleX,hudtext::bossTitleBaseline-12,
                           hudtext::bossTitleScale,true,C2D_Color32f(1,1,1,alpha));
}
void HudRenderer::drawAreaTitle(const AreaTitle& title) const
{
    const float alpha=title.alpha();
    if(!ready()||alpha<=0)return;
    if(title.firstWaterwaysVisit()){
        C2D_Prepare();
        C2D_ImageTint tint;C2D_AlphaImageTint(&tint,alpha);
        for(unsigned i=0;i<areaTitleOrnaments_.size();++i){
            const float y=areatitledata::ornamentY[i]-(i==0?6.f:12.f);
            sprite(frame(areaTitleOrnaments_[i]),areatitledata::ornamentX[i],y,1,&tint,1,true);
        }
        text("ROYAL",200,60,alpha,1.15f,2.5f);
        text("WATERWAYS",200,84,alpha,1.65f,hudtext::regionTitleSpacing);
        return;
    }
    if(title.regionLabel()){
        text(title.regionLabel(),200,215,alpha,hudtext::regionTitleScale,hudtext::regionTitleSpacing);
        return;
    }
    C2D_Prepare();
    C2D_ImageTint tint;C2D_AlphaImageTint(&tint,alpha);
    textcapture::String capture("[area title]");
    const auto part=title.artPart();
    if(!part)sprite(frame(areaTitleParts_[0]),areatitledata::x,areatitledata::y,1,&tint,1,true);
    else for(unsigned i=1+(part-1)*4;i<1+part*4;++i)
        sprite(frame(areaTitleParts_[i]),200,120,1,&tint,1,true);
}
void HudRenderer::observePrompts(const TutorialPrompts* prompts)
{
    if(prompts_!=prompts||(prompts&&promptRevision_!=prompts->revision()))dirty_=true;
    prompts_=prompts;promptRevision_=prompts?prompts->revision():~0u;
}



void HudRenderer::drawBottomPanel(C3D_RenderTarget* target) const
{
    C2D_TargetClear(target,ui::bottomBackgroundColor);
    C2D_SceneBegin(target);C2D_ViewReset();C3D_DepthTest(false,GPU_ALWAYS,GPU_WRITE_COLOR);
    // An opaque backing is independent of the source dialogue texture and its alpha.
    ui::bottomBackground(uiCanvas());
    ui::window(uiCanvas());
}
void HudRenderer::drawMenuDetail(const PauseMenu& menu,const char* room,const HudPresentation* hud,const SimpleMap* map,Vec2 hero) const
{
    if(!ready()||!menu.active())return;
    textcapture::select(0,false);
    const auto c=uiCanvas();
    using Screen=PauseMenu::Screen;
    (void)map;(void)hero;
    if(menu.screen==Screen::Pause&&hud){
        // The global pause dim provides contrast; keep only the title ornament.
        ui::panel(c,{8,96,384,72},false);
        if(room&&!std::strcmp(room,"Tutorial_01")){
            // The remaining title parts belong to Waterways and Basin first visits.
            if(const auto* f=frame(areaTitleParts_[0])){
                const float scale=std::min(336.f/((f->right-f->left)*huddata::pixelsPerUnit),44.f/((f->top-f->bottom)*huddata::pixelsPerUnit));
                sprite(f,200-(f->left+f->right)*huddata::pixelsPerUnit*scale/2,132+(f->top+f->bottom)*huddata::pixelsPerUnit*scale/2,scale);
            }
        }else{
            // Centred on the fleur axis x=200 (was left-aligned at x=24 under the fleur); drawn after
            // C2D_ViewReset in the menu path, so no camera transform applies.
            const char* text=room?saveAreaName(room):"Paused";
            const float width=c.measure(c.user,text,ui::Size::Title);
            c.text(c.user,text,std::round(200.f-width/2),116.f,ui::Size::Title,ui::white);
        }
        // Leave the live room visible; collision segments are not readable map art.
        return;
    }
    if(room)menuArt_.backdrop(menuSeconds_,false);
    c.fill(c.user,{0,0,400,240},0x40000000);
    if(inventoryArt_&&room)inventoryArt_->drawIconFit("Logo",{80,8,240,68});
    char value[96]{};
    const char* name=menu.label(menu.rows.selected);
    if(menu.screen==Screen::Controls)name="Controller";
    if(menu.screen==Screen::Audio&&menu.rows.selected<2)std::snprintf(value,sizeof(value),"%u / 10",unsigned(menu.rows.selected?menu.settings.soundVolume:menu.settings.musicVolume));
    else if(menu.screen==Screen::Video&&menu.rows.selected==0)std::snprintf(value,sizeof(value),"%s",graphicsModeName(menu.settings.graphics));
    else if(menu.screen==Screen::Game&&menu.rows.selected==0)std::snprintf(value,sizeof(value),"English");
    else if(menu.screen==Screen::Options){
        if(menu.rows.selected==0)std::snprintf(value,sizeof(value),"English");
        else if(menu.rows.selected==1)std::snprintf(value,sizeof(value),"Music %u   Sound %u",unsigned(menu.settings.musicVolume),unsigned(menu.settings.soundVolume));
        else if(menu.rows.selected==2)std::snprintf(value,sizeof(value),"%s",graphicsModeName(menu.settings.graphics));
        else if(menu.rows.selected==3)std::snprintf(value,sizeof(value),"Nintendo 3DS");
    }
    ui::detail(c,name,value,menu.description(),{8,80,384,152},false);
}
void HudRenderer::drawMenu(C3D_RenderTarget* target,const PauseMenu& menu) const
{
    if(!ready()||!menu.active())return;
    C2D_SceneBegin(target);C2D_ViewReset();C3D_DepthTest(false,GPU_ALWAYS,GPU_WRITE_COLOR);
    const auto c=menuCanvas();
    using Screen=PauseMenu::Screen;
    const auto screen=menu.screen;
    const bool controls=screen==Screen::Controls,keyboard=screen==Screen::Keyboard;
    const char* heading=controls?"Controller":keyboard?"Keyboard":screen==Screen::Audio?"Audio":
        screen==Screen::Video?"Video":screen==Screen::Game?"Game":screen==Screen::Options?"Options":menu.atMainMenu()?"HOLLOW KNIGHT":"Paused";
    menuArt_.backdrop(menuSeconds_,false);
    if(screen==Screen::Pause)C2D_DrawRectSolid(0,0,0,320,240,C2D_Color32(0,0,0,125));
    if(screen!=Screen::Pause)ui::menuRow(c,{16,6,288,28},heading,nullptr,false);
    if(c.art)c.art(c.user,ui::Art::FrameTop,{64,30,192,14});
    if(controls||keyboard){
        if(controls){
            const char* labels[]={"D-Pad / Circle Pad: Move","B: Jump    X: Nail","Y: Cast / Hold Focus    ZR: Cast","L: Dash    R: Dream Nail","Up / Down: Look / Interact","START: Pause    SELECT: Page"};
            for(unsigned i=0;i<6;++i)ui::wrappedText(c,labels[i],{20,56+i*20.f,280,20});
        }else ui::wrappedText(c,"Use the 3DS buttons and Circle Pad. Keyboard input is not available.",{20,64,280,100});
        ui::menuRow(c,{16,menu.rowY(0)-16,288,28},"Back",nullptr,true);
    }else{
        for(unsigned i=0;i<menu.count();++i){
            if(!menu.rowVisible(i))continue;
            char value[40]{};
            if(screen==Screen::Audio&&i<2)std::snprintf(value,sizeof(value),"< %u >",unsigned(i?menu.settings.soundVolume:menu.settings.musicVolume));
            else if(screen==Screen::Video&&i==0)std::snprintf(value,sizeof(value),"< %s >",graphicsModeName(menu.settings.graphics));
            else if(screen==Screen::Game&&i==0)std::snprintf(value,sizeof(value),"English");
            ui::menuRow(c,{24,menu.rowY(i)-16,272,28},menu.label(i),value,i==menu.rows.selected);
        }
    }
    if(!controls&&!keyboard&&!menu.adjusting()&&c.art)c.art(c.user,ui::Art::FrameBottom,{64,188,192,14});
    if(menu.saveFailed&&!controls&&!keyboard)ui::wrappedText(c,"Save failed - try again",{20,menu.adjusting()?164.f:188.f,280,20});
    const ui::Action actions[]={{"A","Select"},{"B","Back"},{"Left/Right", "Adjust"}};
    ui::promptBar(c,actions,menu.adjusting()?3:2);
}
void HudRenderer::drawCredits(const OpeningCredits& credits) const
{
    if(!ready()||!credits.active())return;
    for(unsigned i=0;i<creditLabels_.size();++i){
        const auto& label=creditsdata::texts[i];
        const float alpha=credits.alpha(label);if(alpha<=0)continue;
        // Reuse the resident LCD font: source-sized baked lines have 4 px ink.
        // Preserve their clocks, with readable captions/names at the right edge.
        const bool caption=label.scale<.6f;
        const float scale=caption?.92f:1.15f;
        const float width=textWidth(label.value,scale);
        text(label.value,390-width*.5f,caption?190.f:211.f,alpha,scale);
    }
}
void HudRenderer::observeInspection(const LoreInspection* inspection)
{
    if(inspection_!=inspection||(inspection&&inspectionRevision_!=inspection->revision()))dirty_=true;
    inspection_=inspection;inspectionRevision_=inspection?inspection->revision():~0u;
}

void HudRenderer::lore() const
{
    if(!inspection_)return;
    const float alpha=inspection_->alpha();
    if(alpha>0){
        const auto canvas=uiCanvas();
        float footerY=190;
        if(canvas.art){
            canvas.art(canvas.user,ui::Art::FrameTop,{64,35,192,16});
        }
        if(inspection_->phase()==InspectionPhase::Reading){
            const char* source=inspection_->text();
            const char* p=source;
            const unsigned visible=inspection_->visibleCharacters();
            constexpr float scale=ui::scale(ui::Size::Body);
            float baseline=66;
            while(*p){
                while(*p==' '||*p=='\n')++p;
                if(!*p)break;
                char line[256]{},shown[256]{};
                unsigned count=0,shownCount=0;
                bool paragraphEnd=false;
                while(*p){
                    const char* word=p;
                    while(*p&&*p!=' '&&*p!='\n')++p;
                    const unsigned length=unsigned(p-word);
                    if(count&&count+1+length<sizeof(line)){
                        line[count]=' ';std::memcpy(line+count+1,word,length);line[count+1+length]=0;
                        if(textWidth(line,scale)>topui::loreBodyWidth){line[count]=0;p=word;break;}
                        ++count;
                        if(unsigned(word-source)<visible)shown[shownCount++]=' ';
                    }
                    if(count+length>=sizeof(line))break;
                    std::memcpy(line+count,word,length);count+=length;line[count]=0;
                    for(unsigned i=0;i<length&&unsigned(word+i-source)<visible;++i)shown[shownCount++]=word[i];
                    unsigned newlines=0;
                    while(*p==' '||*p=='\n'){if(*p=='\n')++newlines;++p;}
                    if(newlines>1){paragraphEnd=true;break;}
                }
                shown[shownCount]=0;
                text(shown,160-(textWidth(line,scale)-textWidth(shown,scale))*.5f,baseline,alpha,scale);
                baseline+=ui::lineHeight(ui::Size::Body)+(paragraphEnd?8.f:0.f);
            }
            footerY=std::min(205.f,baseline+3.f);
        }
        if(canvas.art)canvas.art(canvas.user,ui::Art::FrameBottom,{64,footerY,192,16});
        if(inspection_->phase()==InspectionPhase::Reading&&inspection_->pageComplete())
            sprite(frame(inspection_->hasNextPage()?loreArrow_:loreStop_),160,footerY+8,1.3f);
    }else if(inspection_->prompt()){
        prompts(HudOverlay::Inspect,true);
    }
}

void HudRenderer::button(const char* label,float x,float y,float alpha) const
{
    // 3DS face-button adaptation of ActionButtonIcon; Y=Focus, X=Nail, B=Jump.
    C2D_ImageTint tint{};C2D_PlainImageTint(&tint,C2D_Color32f(1,1,1,alpha),0);
    sprite(frame(promptButton_),x,y,1,&tint);
    text(label,x,y+5,alpha);
}
void HudRenderer::prompts(HudOverlay overlay, bool lorePrompt) const
{
    if(lorePrompt){
        text("Inspect",160,193,1,1.2f);
        button("^",160,213,1);
        return;
    }
    if(!prompts_)return;
    const float a=prompts_->alpha();
    using namespace promptlayout;
    if(overlay==HudOverlay::Reminder){
        C2D_ImageTint tint{};C2D_PlainImageTint(&tint,C2D_Color32f(1,1,1,a),0);
        // Scale the authored backboard to the measured native on-screen size.
        // Scale 1 drew a 60x61 px disk that covered only the middle of the row.
        if(const SpriteFrame* board=frame(promptBackboard_)){
            const float baseW=(board->right-board->left)*huddata::pixelsPerUnit;
            const float baseH=(board->top-board->bottom)*huddata::pixelsPerUnit;
            const float sx=reminderBackboardWidth/baseW;
            const float sy=reminderBackboardHeight/baseH;
            sprite(board,centerX,reminderBackboardY,sx,&tint,sy/sx);
        }
        const float width=textWidth(prompts_->text(),reminderScale);
        const float left=centerX-(2*buttonRadius+reminderGap+width)*.5f;
        const float iconX=left+buttonRadius;
        button(prompts_->button(),iconX,reminderY,a);
        if(prompts_->hold())text("HOLD",iconX,holdBaseline,a,holdScale);
        text(prompts_->text(),left+2*buttonRadius+reminderGap+width*.5f,
             reminderBaseline,a,reminderScale);
    }
    const float f=prompts_->tabletAlpha();
    if(overlay==HudOverlay::Focus){
        C2D_ImageTint tint{};C2D_PlainImageTint(&tint,C2D_Color32f(1,1,1,f),0);
        const bool withLore=inspection_&&inspection_->alpha()>0;
        sprite(frame(focusPanel_),centerX,withLore?loreFocusArtY:focusArtY,
               withLore?loreFocusArtScale:focusArtScale,&tint);
        text(hk3ds::text::resolve("@Prompts/PROMPT_FOCUS_EXPLAINER1A"),centerX,focusLine1,f,focusTextScale);
        text(hk3ds::text::resolve("@Prompts/PROMPT_FOCUS_EXPLAINER1B"),centerX,focusLine2,f,focusTextScale);
        text("HOLD",focusHoldX,focusHoldBaseline,f,focusHoldScale);
        button("Y",focusButtonX,focusButtonY,f);
        text(hk3ds::text::resolve("@Prompts/PROMPT_FOCUS_EXPLAINER2"),centerX,focusLastLine,f,focusTextScale);
    }
}
const Clip* HudRenderer::maskClip(const HudMask& mask) const
{
    const bool blue=mask.kind==HudMaskKind::Blue||
        (mask.animation==HudMaskAnimation::Break&&mask.previous==HudMaskKind::Blue);
    return mask.animation==HudMaskAnimation::Break?(blue?blueBreak_:healthBreak_):
        mask.animation==HudMaskAnimation::Refill?(blue?blueAppear_:healthRefill_):
        mask.kind==HudMaskKind::Empty?healthEmpty_:(blue?blueIdle_:healthIdle_);
}
void HudRenderer::advanceMenu(float seconds)
{
    if(ready()&&std::isfinite(seconds)&&seconds>0)menuSeconds_+=std::min(seconds,.1f);
}
void HudRenderer::advance(float seconds,const HudPresentation& hud)
{
    if(!ready()||!std::isfinite(seconds)||seconds<=0)return;
    const float next=seconds_+seconds;
    // Source idle clips are independent of changes to health/soul/geo. A
    // repeated source frame does not require a bottom draw or LCD transfer.
    const auto changed=[&](const Clip* clip){
        return clip&&clip->count>1&&frame(clip,seconds_)!=frame(clip,next);
    };
    const bool pending=hud.snapshot().geo!=hud.displayedGeo();
    if(changed(pending?coinGet_:coinIdle_)||changed(soulEyes_)||changed(frame_))dirty_=true;
    const auto* liquidFrame=frame(hud.liquidAnimation()==HudLiquidAnimation::Shrink?liquidShrink_:liquidIdle_,hud.liquidSeconds());
    if(hud.displayedSoul()>1&&liquidFrame!=lastLiquidFrame_)dirty_=true;
    lastLiquidFrame_=liquidFrame;
    for(const auto& mask:hud.masks())
        if(mask.kind!=HudMaskKind::Hidden&&mask.animation==HudMaskAnimation::None&&changed(maskClip(mask)))dirty_=true;
    seconds_=next;
}
ShopMenuCanvas HudRenderer::shopCanvas() const {
    return {uiCanvas(),menuCanvas(),const_cast<HudRenderer*>(this),
        [](void* p,const char* key,ui::Rect r){static_cast<HudRenderer*>(p)->shopIcon(key,r);},
        [](void* p,ui::Rect r){const auto* self=static_cast<HudRenderer*>(p);if(self->inventoryArt_)self->inventoryArt_->drawSelection(r);}};
}
ui::DialogCanvas HudRenderer::dialogCanvas() const {
    return {uiCanvas(),menuCanvas(),const_cast<HudRenderer*>(this),
        [](void* p,const char* key,ui::Rect r){static_cast<HudRenderer*>(p)->shopIcon(key,r);}};
}
void HudRenderer::shopIcon(const char* key,ui::Rect r) const {
    if(!key)return;
    if(!std::strcmp(key,"Geo")){
        if(inventoryArt_&&inventoryArt_->drawIconFit("Geo",r))return;
        sprite(frame(coinIdle_),r.x+r.w*.5f,r.y+r.h,.5f);return;
    }
    if(!shopArt_)return;
    const Clip* clip=nullptr;
    for(const auto& item:slyItems)if(!std::strcmp(item.flag,key)){clip=shopArt_->bank.find(item.flag);break;}
    AnimationPlayer icon;icon.play(clip);
    const auto* f=icon.current(shopArt_->bank);
    if(!f)return;
    const auto page=C2D_SpriteSheetGetImage(shopArt_->page,0);
    if(!page.tex)return;
    Tex3DS_SubTexture sub{};sub.width=f->width;sub.height=f->height;
    sub.left=float(f->x)/page.tex->width;sub.right=float(f->x+f->width)/page.tex->width;
    sub.top=1-float(f->y)/page.tex->height;sub.bottom=1-float(f->y+f->height)/page.tex->height;
    const float scale=std::min(r.w/f->width,r.h/f->height);
    C2D_DrawImageAt({page.tex,&sub},r.x+(r.w-f->width*scale)*.5f,
                    r.y+(r.h-f->height*scale)*.5f,0,nullptr,scale,scale);
}
void HudRenderer::drawSlyShop(const SlyShop& shop,unsigned geo,bool details) const
{
    // The Shop prompt near Sly is the top-screen Arrow Prompt (ArrowPrompt), not drawn here.
    if(!shop.open)return;
    C2D_ViewReset();
    std::array<ShopMenuItem,8> stock{};unsigned count=0;
    for(unsigned i=0;i<shop.count&&i<shop.stock.size();++i){
        if(shop.stock[i]>=slyItems.size())continue;
        const auto& item=slyItems[shop.stock[i]];
        stock[count++]={item.name,hk3ds::text::resolve(item.description),item.flag,item.price};
    }
    const ShopMenuView view{"Sly",stock.data(),count,shop.selected,geo,shop.confirm,shop.yes,shop.textPage,shop.message};
    // Old Iselda layout: the list window is the bottom screen, the item text the top screen.
    if(details)drawShopMenuBottom(shopCanvas(),view);
    else shop.setVisibleTextPages(drawShopMenuTop(shopCanvas(),view).pages);
}
void HudRenderer::drawIseldaShop(const Cartographer& npc,unsigned geo,bool details) const
{
    // Iselda's purchase step (Prompt) uses the same shop menu as Sly: item text on the top screen, window on the bottom.
    if(!npc.shop||npc.dialogue!=Cartographer::Dialogue::Prompt)return;
    C2D_ViewReset();
    const auto item=npc.shopItem();
    const auto view=npc.shopMenu(item,geo);
    if(details)drawShopMenuBottom(shopCanvas(),view);
    else drawShopMenuTop(shopCanvas(),view);
}
void HudRenderer::drawRouteToll(const TollMenu& menu,unsigned geo,bool details) const
{
    // The Inspect prompt is the top-screen Arrow Prompt (ArrowPrompt); the choice is the shared bottom dialog.
    if(!details||!menu.open)return;
    C2D_ViewReset();
    ui::bottomBackground(uiCanvas());
    if(menu.crest){
        ui::ChoiceDialog dialog{menu.question,0,menu.yes,true,nullptr};
        dialog.showPrice=false;dialog.showChoices=!menu.refusal;
        ui::choiceDialog(dialogCanvas(),dialog);return;
    }
    char info[48];std::snprintf(info,sizeof(info),"You have %u Geo",geo);
    ui::choiceDialog(dialogCanvas(),{menu.question?menu.question:"",
                                     menu.price,menu.yes,geo>=menu.price,menu.message[0]?menu.message:info});
}

void HudRenderer::draw(C3D_RenderTarget* target,const HudPresentation& hud,const FuryChest* chest,const AnimationBank* items,const HeroRenderer* renderer,const ConditionalEvents* events,const AnimationBank* eventArt,const HeroRenderer* eventRenderer,const WorldQuestDialogue* quest,bool nativeVitals)
{
    (void)eventArt;
    (void)eventRenderer;
    const bool showEvent=events&&(events->dialogue!=EventDialogue::None||events->listenPrompt);
    const bool showQuest=quest&&(quest->visible()||quest->prompt);
    if(showQuest||showEvent||(showEvent||showQuest)!=eventVisible_||(events&&events->textPage()!=eventPage_))dirty_=true;
    eventVisible_=showEvent||showQuest;eventPage_=events?events->textPage():~0u;
    const bool showItem=chest&&(chest->pickup()==FuryPickupPhase::Display||chest->pickup()==FuryPickupPhase::WaitButton);
    const bool showPrompt=false; // Arrow Prompt is drawn at its source world anchor.
    const auto overlay=hudOverlay(showItem,showPrompt,prompts_?prompts_->tabletAlpha():0,prompts_?prompts_->alpha():0);
    if(showItem||showItem!=itemVisible_||showPrompt!=promptVisible_)dirty_=true;
    itemVisible_=showItem;promptVisible_=showPrompt;
    if(!ready()||!target)return;
    C2D_SceneBegin(target);
    C2D_ViewReset();
    // The HUD now overlays the world. The old vignette alone cannot suppress
    // the bright tablet glyph behind both modal text groups. Fade a shared
    // scrim with the original panel faders, before drawing vitals and text.
    const float loreAlpha=inspection_&&hudLoreVisible(overlay,inspection_->locked())?inspection_->alpha():0;
    const float focusAlpha=overlay==HudOverlay::Focus&&prompts_?prompts_->tabletAlpha():0;
    const float dim=std::max(loreAlpha,focusAlpha)*promptlayout::modalDimOpacity;
    if(dim>0)C2D_DrawRectSolid(0,0,0,400,240,C2D_Color32(0,0,0,static_cast<u8>(255*dim)));
    if(!nativeVitals){
        C2D_ViewTranslate(topui::vitalsX,topui::vitalsY);
        C2D_ViewScale(topui::vitalsScale,topui::vitalsScale);
    }
    C3D_DepthTest(false,GPU_ALWAYS,GPU_WRITE_COLOR);
    sprite(frame(hud.snapshot().soulLimited()?crackedFrame_:frame_,seconds_),huddata::frameX,huddata::frameY);
    liquid(target,hud);
    if(hud.soulEyesAlpha()>0){
        C2D_ImageTint tint{};C2D_PlainImageTint(&tint,C2D_Color32f(1,1,1,hud.soulEyesAlpha()),0);
        sprite(frame(soulEyes_,seconds_),huddata::eyesX,huddata::eyesY,1,&tint,hud.soulEyesScale());
    }
    if(hud.snapshot().soulLimited())sprite(frame(soulLimiter_),huddata::soulX,huddata::soulY);
    soulFeedback(hud);
    limiterFeedback(hud);
    refillFeedback(hud);
    unsigned slot=0;
    for(const auto& mask:hud.masks()){
        if(mask.kind==HudMaskKind::Hidden&&mask.animation==HudMaskAnimation::None)continue;
        const auto* clip=maskClip(mask);
        const float clock=mask.animation==HudMaskAnimation::None?seconds_:mask.elapsed;
        sprite(frame(clip,clock),huddata::firstMaskX+slot*huddata::maskStep,huddata::maskY,huddata::maskScale);
        ++slot;
    }
    const auto pending=std::int64_t(hud.snapshot().geo)-hud.displayedGeo();
    if(hud.geoVisible()){
        const float reveal=hud.geoRevealSeconds();
        const auto* coin=reveal<.25f?coinAppear_:reveal<.45f?coinGetOnce_:
            pending&&hud.geoCountingSeconds()>=0?coinGet_:coinIdle_;
        sprite(frame(coin,reveal<.25f?reveal:reveal<.45f?reveal-.25f:std::max(0.f,hud.geoCountingSeconds())),huddata::coinX,huddata::coinY);
        number(hud.displayedGeo(),huddata::geoTextX,huddata::geoTextY,hud.geoTextAlpha());
    }
    if(hud.pendingGeoAlpha()>0&&hud.geoVisible()){
        C2D_ImageTint tint{};C2D_AlphaImageTint(&tint,hud.pendingGeoAlpha());
        sprite(frame(plus_),huddata::pendingTextX,huddata::pendingTextY,1,&tint,1,true);
        number(hud.pendingGeo(),huddata::pendingTextX+huddata::geoAdvance[10]+huddata::geoAdvance[12],huddata::pendingTextY,hud.pendingGeoAlpha());
    }
    C2D_ViewReset();
    // Cover vitals as well as the world while the inspection is open. The
    // Focus instruction is drawn afterwards and remains available to read.
    if(loreAlpha>0)C2D_DrawRectSolid(0,0,0,400,240,C2D_Color32(0,0,0,static_cast<u8>(255*loreAlpha)));
    C2D_ViewTranslate(topui::panelX,topui::panelY);
    C2D_ViewScale(topui::panelScale,topui::panelScale);
    if(overlay==HudOverlay::Item&&items&&renderer){
        const auto& icon=chest->draws()[static_cast<unsigned>(FuryPart::Icon)];
        const float age=chest->displaySeconds();
        const auto alpha=[&](FuryTutorialPart part){return furyTutorialAlpha(part,age);};
        // LCD reflow overlaps the HUD: retain the existing opaque .2s scrim.
        const float backdrop=std::min(1.f,age/.2f);
        C2D_DrawRectSolid(10,17,0,300,218,C2D_Color32(0,0,0,static_cast<u8>(255*backdrop)));
        for(const auto part:{FuryTutorialPart::Divider,FuryTutorialPart::Illustration}){
            const float fade=alpha(part);if(fade<=0)continue;
            AnimationPlayer art;art.play(items->find(part==FuryTutorialPart::Divider?"Fury Divider":"Fury Illustration"));
            renderer->drawAnimation(art,*items,160,126,1,1,1,fade);
        }
        if(alpha(FuryTutorialPart::Icon)>0)renderer->drawAnimation(icon.player,*items,160,126,1,1,1,alpha(FuryTutorialPart::Icon));
        for(unsigned row=0;row<8;++row){
            char name[32];std::snprintf(name,sizeof(name),"Fury Message %u",row);
            const auto* clip=items->find(name);if(!clip)break;
            const float fade=furyTutorialRowAlpha(row,age);if(fade<=0)continue;
            AnimationPlayer line;line.play(clip);renderer->drawAnimation(line,*items,160,126,1,1,1,fade);
        }
    }
    if(!inspection_||!inspection_->locked())prompts(overlay);
    if(hudLoreVisible(overlay,inspection_&&inspection_->locked()))lore();
    if((showEvent||showQuest)&&hudEventVisible(overlay)&&eventArt&&eventRenderer){
        // Quest dialogue occupies the original upper-screen panel; other event
        // messages retain their existing LCD composition.
        if(showQuest&&quest->locked())C2D_ViewReset();
        const bool questBox=showQuest&&quest->locked();
        const bool tollBox=questBox&&quest->tollLayout;
        const float panelCenter=questBox?questdialogue::center:dialoguedata::center;
        const float textLeft=tollBox?tolldialogue::left:questBox?questdialogue::left:dialoguedata::left;
        const float lineHeight=tollBox?tolldialogue::lineHeight:questBox?questdialogue::lineHeight:dialoguedata::lineHeight;
        const auto drawText=[&](const char* value,float baseline,bool body=false){
            const auto canvas=uiCanvas();const char* start=value;
            while(*start){const char* end=std::strchr(start,'\n');if(!end)end=start+std::strlen(start);
                std::array<char,768> line{};const auto count=std::min<std::size_t>(line.size()-1,static_cast<std::size_t>(end-start));
                std::memcpy(line.data(),start,count);line[count]=0;
                const float width=canvas.measure(canvas.user,line.data(),ui::Size::Body);
                const float x=body?textLeft:(questBox?panelCenter:160.f)-width/2;
                canvas.text(canvas.user,line.data(),x,baseline-ui::baselineOffset(ui::Size::Body),ui::Size::Body,ui::white);
                if(!*end)break;
                start=end+1;baseline+=lineHeight;
            }
        };
        if((showQuest&&quest->locked())||(showEvent&&events->dialogue!=EventDialogue::None)){
            // Original shared flourishes and left-aligned Perpetua body.
            // Modal item messages own this area while their overlay is visible.
            const auto decoration=[&](const char* name,float x,float y,float scale=1){
                AnimationPlayer player;player.play(eventArt->find(name));
                eventRenderer->drawAnimation(player,*eventArt,x,y,16,scale,scale);
            };
            if(questBox)sprite(frame(loreBackboard_),panelCenter,tollBox?tolldialogue::backboardY:questdialogue::backboardY,
                               tollBox?tolldialogue::backboardScale:questdialogue::backboardScale);
            decoration("Dialogue Top",panelCenter,
                       tollBox?tolldialogue::top:questBox?questdialogue::top:dialoguedata::top,
                       tollBox?tolldialogue::topScale:questBox?questdialogue::topScale:1);
            if(showQuest){
                if(quest->phase()==WorldQuestDialogue::Phase::Reading){
                    std::array<char,768> visible{};
                    std::snprintf(visible.data(),visible.size(),"%s",quest->text());
                    visible[std::min<unsigned>(quest->visibleCharacters(),visible.size()-1)]=0;
                    drawText(visible.data(),tollBox?tolldialogue::baseline:questdialogue::baseline,true);
                }
            }else drawText(events->text(),dialoguedata::baseline,true);
            decoration("Dialogue Bottom",panelCenter,
                       tollBox?tolldialogue::bottom:questBox?questdialogue::bottom:dialoguedata::bottom,
                       tollBox?tolldialogue::bottomScale:questBox?questdialogue::bottomScale:1);
            if(!showQuest||quest->pageComplete())
                decoration((showQuest?quest->hasNextPage():events->hasNextPage())?"Dialogue Arrow":"Dialogue Stop",
                           tollBox?tolldialogue::markerX:questBox?questdialogue::markerX:dialoguedata::markerX,
                           tollBox?tolldialogue::markerY:questBox?questdialogue::markerY:dialoguedata::markerY,
                           tollBox?tolldialogue::markerScale:questBox?questdialogue::markerScale:1.5f);
            if(showQuest&&quest->choiceVisible&&quest->pageComplete()){
                char choices[144];
                std::snprintf(choices,sizeof(choices),"%s%s     %s%s",quest->choiceYes?"> ":"",quest->yesLabel,
                              quest->choiceYes?"":"> ",quest->noLabel);
                drawText(choices,tollBox?tolldialogue::choicesY:questBox?questdialogue::bottom-18.f:dialoguedata::controls);
            }
            if(!showQuest)drawText("B / X",dialoguedata::controls);
        }
        // User 2026-11-12: no "<Action> - Up" text here; the world prompt is the top-screen Arrow Prompt.
    }
    C2D_ViewReset();
    dirty_=false;++draws_;
}
}
