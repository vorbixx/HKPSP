#include "ui.hpp"
#include "ui_window.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <cstdio>

namespace hk3ds::ui {
std::uint64_t textIdentity(const char* text) {
    std::uint64_t result=14695981039346656037ull;
    if(text)for(;*text;++text){result^=static_cast<unsigned char>(*text);result*=1099511628211ull;}
    return result;
}
void panel(Canvas c,Rect r,bool backing) {
    if(backing)c.fill(c.user,r,0xeb000000);
    if(c.art&&r.h>=40){
        c.art(c.user,Art::FrameTop,{r.x+r.w/2-60,r.y,120,10});
        if(r.h>=80)c.art(c.user,Art::FrameBottom,{r.x+r.w/2-60,r.y+r.h-10,120,10});
    }
}
void label(Canvas c,const char* text,float x,float baseline,bool centred,std::uint32_t colour) {
    const float width=c.measure(c.user,text,Size::Heading);
    const float left=centred?x-width*.5f:x,top=baseline-baselineOffset(Size::Heading);
    c.text(c.user,text,left,top,Size::Heading,colour);
    c.text(c.user,text,left+.5f,top,Size::Heading,colour);
}
void window(Canvas c,const char* title,const char* right) {
    c.fill(c.user,windowRect,windowColor);
    panel(c,windowRect,false);
    if(title)label(c,title,26,29);
    if(right)label(c,right,265,29,true);
}
void selectionRow(Canvas c,float y,bool selected,const char* name,const char* value,bool iconRoom) {
    if(selected){
        c.fill(c.user,{12,y-20,296,46},highlightColor);
        label(c,">",17,y+2);
    }
    if(name)label(c,name,iconRoom?65.f:36.f,y-3);
    if(value)label(c,value,268,y-3,true);
}
void choiceDialog(const DialogCanvas& d,const ChoiceDialog& v) {
    const Canvas& c=d.body;
    window(c);
    // Question: greedy wrap, each line centred, Heading size.
    const char* text=v.question?v.question:"";
    float baseline=58;
    for(std::size_t offset=0;text[offset]&&baseline<(v.showPrice?100:136);){
        char line[160]{};std::size_t length=0,lastSpace=0;
        while(text[offset+length]&&length<sizeof(line)-1){
            line[length]=text[offset+length];line[length+1]=0;
            if(c.measure(c.user,line,Size::Heading)>windowRect.w-32){line[length]=0;break;}
            if(line[length]==' ')lastSpace=length;
            ++length;
        }
        if(text[offset+length]&&lastSpace)length=lastSpace;
        line[length]=0;
        label(c,line,160,baseline,true);
        offset+=length;while(text[offset]==' ')++offset;
        baseline+=lineHeight(Size::Heading);
    }
    if(v.showPrice){
    char price[24];std::snprintf(price,sizeof(price),"%u",v.price);
    const float priceWidth=c.measure(c.user,price,Size::Heading);
    const float left=160-(priceWidth+24)*.5f;
    if(d.icon)d.icon(d.user,"Geo",{left,100,16,20});
    label(c,price,left+24,118);
    }
    if(v.showChoices){
    constexpr std::uint32_t grey=0xff7a746e;
    label(c,v.yes?"> Yes":"Yes",108,150,true,v.affordable?(v.yes?white:secondary):grey);
    label(c,v.yes?"No":"> No",212,150,true,v.yes?secondary:white);
    }
    const char* info=v.affordable?v.info:"Not enough Geo.";
    if(info&&*info)label(c,info,160,178,true,secondary);
    char confirm[40],cancel[40];
    std::snprintf(confirm,sizeof(confirm),"%s: Confirm",v.confirmKey?v.confirmKey:"A");
    std::snprintf(cancel,sizeof(cancel),"%s: Cancel",v.cancelKey?v.cancelKey:"B");
    char hint[96];std::snprintf(hint,sizeof(hint),"%s   %s",confirm,cancel);
    if(!v.showChoices)std::snprintf(hint,sizeof(hint),"A: Continue");
    const char* lines[]={hint};
    hintLines(c,lines,1);
}
namespace {
// Listed (opened) stations in order; maps a list row to the station index.
unsigned openedIndex(const StagMenuView& v,unsigned row,bool& found) {
    unsigned seen=0;found=false;
    for(unsigned i=0;i<v.count;++i)if(v.stations[i].opened){if(seen==row){found=true;return i;}++seen;}
    return 0;
}
unsigned openedFirst(const StagMenuView& v) {
    unsigned before=0,total=0;
    for(unsigned i=0;i<v.count;++i)if(v.stations[i].opened){if(i<v.selected)++before;++total;}
    return total>stagVisibleRows?std::min(before>1?before-1:0u,total-stagVisibleRows):0u;
}
}
void stagStationList(const DialogCanvas& d,const StagMenuView& v) {
    const Canvas& c=d.body;
    window(c,"Stations");
    const unsigned first=openedFirst(v);
    for(unsigned row=0;row<stagVisibleRows;++row){
        bool found;const unsigned i=openedIndex(v,first+row,found);
        if(!found)break;
        const auto& st=v.stations[i];
        const float y=stagRowFirstBaseline+row*stagRowStep;
        selectionRow(c,y,i==v.selected,st.name,nullptr,true);
        if(st.current&&d.icon)d.icon(d.user,"StagCurrent",{19,y-20,32,32});
    }
    const Action actions[]={{"A","Travel"},{"B","Cancel"}};
    promptBar(c,actions,2);
}
int stagStationAt(const StagMenuView& v,float x,float y) {
    if(x<12||x>308)return -1;
    const unsigned first=openedFirst(v);
    for(unsigned row=0;row<stagVisibleRows;++row){
        const float baseline=stagRowFirstBaseline+row*stagRowStep;
        if(y>=baseline-20&&y<baseline+16){
            bool found;const unsigned i=openedIndex(v,first+row,found);
            return found?int(i):-1;
        }
    }
    return -1;
}
void stagStationMap(const DialogCanvas& d,const StagMenuView& v) {
    const Canvas& c=d.body;
    c.fill(c.user,{0,0,400,240},0x99000000);
    if(c.art){
        c.art(c.user,Art::FrameTop,{120,6,160,20});
        c.art(c.user,Art::FrameBottom,{120,214,160,20});
    }
    for(unsigned i=0;i<v.count;++i){
        const auto& st=v.stations[i];
        if(!st.opened)continue;   // unopened stations stay hidden, as in the original
        const float x=stagMapRect.x+st.mapX*stagMapRect.w-8,y=stagMapRect.y+st.mapY*stagMapRect.h-8;
        if(d.icon){
            d.icon(d.user,st.current?"StagCurrent":"Stag",{x,y,16,16});
            if(i==v.selected)d.icon(d.user,"StagCursor",{x-6,y-6,28,28});
        }
    }
}
void bottomBackground(Canvas c) {
    c.fill(c.user,{0,0,320,240},bottomBackgroundColor);
}
void unavailableMap(Canvas c) {
    wrappedText(c,"Map",{24,76,272,28},Size::Heading);
    wrappedText(c,"You do not have a map for this area.",{24,112,272,60});
    wrappedText(c,"SELECT changes pages.",{24,176,272,20},Size::Body,secondary);
}
void menuRow(Canvas c,Rect r,const char* name,const char* value,bool selected) {
    char label[160]{};unsigned n=0;
    const auto append=[&](const char* text){
        if(!text)return;
        for(;*text&&n+1<sizeof(label);++text){
            const char ch=*text;
            label[n++]=ch>='a'&&ch<='z'?char(ch-'a'+'A'):ch;
        }
    };
    append(name);
    if(value&&*value){append("  ");append(value);}
    const float width=c.measure(c.user,label,Size::Body);
    const float x=r.x+(r.w-width)*.5f,y=r.y+(r.h-lineHeight(Size::Body))*.5f;
    c.text(c.user,label,x,y,Size::Body,selected?white:secondary);
    if(selected&&c.art){
        c.art(c.user,Art::Pointer,{x-24,y-2,16,16});
        c.art(c.user,Art::PointerRight,{x+width+8,y-2,16,16});
    }
}

TextResult wrappedText(Canvas c,const char* text,Rect r,Size size,std::uint32_t color) {
    if(!text)return {0,0,false};
    std::size_t offset=0; unsigned row=0;
    const auto limit=unsigned(std::max(0.f,std::floor(r.h/lineHeight(size))));
    while(text[offset]&&row<limit&&r.w>0) {
        char line[256]{};std::size_t length=0,lastSpace=0;
        while(text[offset+length]&&text[offset+length]!='\n'&&length<sizeof(line)-1) {
            // Measure whole UTF-8 scalars: a strict Canvas must never see a byte prefix.
            std::size_t end=length+1;
            while((static_cast<unsigned char>(text[offset+end])&0xc0)==0x80)++end;
            if(end>=sizeof(line))break;
            std::memcpy(line+length,text+offset+length,end-length);line[end]=0;
            if(c.measure(c.user,line,size)>r.w){line[length]=0;break;}
            if(line[length]==' ')lastSpace=length;
            length=end;
        }
        if(!length&&text[offset]!='\n')return {offset,row,true};
        if(text[offset+length]&&text[offset+length]!='\n'&&text[offset+length]!=' '&&lastSpace)length=lastSpace;
        line[length]=0;
        c.text(c.user,line,std::round(r.x),std::round(r.y+row*lineHeight(size)),size,color);
        offset+=length;
        if(text[offset]=='\n')++offset;
        else while(text[offset]==' ')++offset;
        ++row;
    }
    return {offset,row,text[offset]!=0};
}
TextResult readingText(Canvas c,const char* text,Rect r,unsigned visible,std::uint32_t color) {
    if(!text)return {0,0,false};
    struct Reading {Canvas canvas;const char* source;Rect rect;std::size_t offset;unsigned visible;};
    Reading state{c,text,r,0,visible};
    Canvas reveal{&state,
        [](void* raw,const char* value,Size size){
            auto& s=*static_cast<Reading*>(raw);return s.canvas.measure(s.canvas.user,value,size);
        },
        [](void* raw,const char* value,float,float y,Size size,std::uint32_t tint){
            auto& s=*static_cast<Reading*>(raw);
            const auto length=std::strlen(value);
            auto shown=std::min(length,s.visible>s.offset?std::size_t(s.visible)-s.offset:0);
            // A reveal boundary may fall inside a multibyte scalar.
            while(shown&&(static_cast<unsigned char>(value[shown])&0xc0)==0x80)--shown;
            char prefix[256]{};std::memcpy(prefix,value,shown);
            const float x=std::round(s.rect.x+(s.rect.w-s.canvas.measure(s.canvas.user,value,size))*.5f);
            s.canvas.text(s.canvas.user,prefix,x,y,size,tint);
            s.offset+=length;
            if(s.source[s.offset]=='\n')++s.offset;
            else while(s.source[s.offset]==' ')++s.offset;
        },nullptr};
    return wrappedText(reveal,text,r,Size::Body,color);
}
PageResult pagedText(Canvas c,const char* text,Rect r,unsigned page,Size size) {
    if(!text||!*text)return {0,1};
    Canvas measure=c;measure.text=[](void*,const char*,float,float,Size,std::uint32_t){};
    unsigned pages=0;
    std::size_t offset=0;
    do {
        const auto result=wrappedText(measure,text+offset,r,size);
        ++pages;
        if(!result.consumed)break;
        offset+=result.consumed;
    }while(text[offset]);
    page=std::min(page,pages-1);offset=0;
    for(unsigned i=0;i<page;++i)offset+=wrappedText(measure,text+offset,r,size).consumed;
    wrappedText(c,text+offset,r,size);
    return {page,pages};
}
void title(Canvas c,const char* text,Rect r) {
    assert(r.h>=lineHeight(Size::Title)+12);
    wrappedText(c,text,{r.x+16,r.y+12,r.w-32,r.h-12},Size::Title);
}
TextResult detail(Canvas c,const char* name,const char* value,const char* description,Rect r,bool backing) {
    panel(c,r,backing);
    wrappedText(c,name,{r.x+16,r.y+12,r.w-32,28},Size::Title);
    const bool hasValue=value&&*value;
    if(hasValue)wrappedText(c,value,{r.x+16,r.y+44,r.w-32,20},Size::Body,secondary);
    const float top=hasValue?72.f:44.f;
    return wrappedText(c,description,{r.x+16,r.y+top,r.w-32,r.h-top-16});
}
void listRow(Canvas c,Rect r,const char* name,const char* value,bool selected,const char* icon) {
    if(selected)c.fill(c.user,r,highlightColor);
    if(selected&&c.art)c.art(c.user,Art::Pointer,{r.x+5,r.y+8,18,12});
    const auto color=selected?white:secondary;
    if(icon)c.text(c.user,icon,r.x+8,r.y+6,Size::Body,color);
    const float left=r.x+(icon||c.art?32:8);
    const float available=std::max(0.f,r.x+r.w-8-left);
    const float right=value?std::min(c.measure(c.user,value,Size::Body),available):0;
    wrappedText(c,name,{left,r.y+6,std::max(0.f,r.w-(left-r.x)-right-20),lineHeight(Size::Body)},Size::Body,color);
    if(value)wrappedText(c,value,{r.x+r.w-right-8,r.y+6,right,lineHeight(Size::Body)},Size::Body,color);
}
float hintLines(Canvas c,const char* const* lines,unsigned count) {
    if(!c.text||!c.measure)return 0;
    for(unsigned i=0;i<count;++i){
        if(!lines[i]||!*lines[i])continue;
        const float width=c.measure(c.user,lines[i],Size::Heading);
        const float baseline=hintLastBaseline-hintLineStep*float(count-1-i);
        const float x=160-width*.5f,y=baseline-baselineOffset(Size::Heading);
        // The old Iselda labels were struck twice, half a pixel apart, to keep the serif face legible.
        c.text(c.user,lines[i],x,y,Size::Heading,white);
        c.text(c.user,lines[i],x+.5f,y,Size::Heading,white);
    }
    return hintLineStep*float(count)+14.f;
}
float promptBar(Canvas c,const Action* actions,unsigned count,float width) {
    char rows[4][96]{};const char* row[4]{};unsigned n=0,used=0;
    const auto limit=width-32;
    for(unsigned i=0;i<count&&n<4;++i){
        const char* key=actions[i].key?actions[i].key:"";
        if(!std::strcmp(key,"Left/Right"))key="L/R";
        else if(!std::strcmp(key,"Up/Down"))key="U/D";
        char piece[72];
        if(*key)std::snprintf(piece,sizeof(piece),"%s: %s",key,actions[i].label?actions[i].label:"");
        else std::snprintf(piece,sizeof(piece),"%s",actions[i].label?actions[i].label:"");
        char joined[96];
        std::snprintf(joined,sizeof(joined),"%s%s%s",rows[n],used?"   ":"",piece);
        if(used&&c.measure&&c.measure(c.user,joined,Size::Heading)>limit){
            if(++n>=4)break;
            used=0;std::snprintf(joined,sizeof(joined),"%s",piece);
        }
        std::snprintf(rows[n],sizeof(rows[n]),"%s",joined);used=1;row[n]=rows[n];
    }
    const unsigned total=n+(used?1u:0u);
    for(unsigned i=0;i<total;++i)row[i]=rows[i];
    return hintLines(c,row,total);
}

TextResult dialogue(Canvas c,const char* speaker,const char* body,const char* action) {
    const Action actions[]={{"A",action},{"B","Close"}};
    Canvas measure=c;
    measure.text=[](void*,const char*,float,float,Size,std::uint32_t){};
    measure.fill=[](void*,Rect,std::uint32_t){};
    measure.art=nullptr;
    const float bodyHeight=std::min(144.f,std::max(0.f,240-promptBar(measure,actions,2)-52));
    panel(c,{8,10,304,230});
    wrappedText(c,speaker,{20,20,280,24},Size::Heading);
    const auto result=wrappedText(c,body,{20,52,280,bodyHeight});
    promptBar(c,actions,2);return result;
}
unsigned firstVisible(unsigned selected,unsigned first,unsigned count,unsigned visible) {
    if(!count||!visible)return 0;
    selected=std::min(selected,count-1);
    first=std::min(first,count>visible?count-visible:0);
    if(selected<first)return selected;
    return selected>=first+visible?selected-visible+1:first;
}
}

#ifdef __3DS__
#include "hud_renderer.hpp"
#include "tutorial_prompt_layout.hpp"
#include "ability_pickup.hpp"
#include "pause_menu.hpp"
#include "fury_chest.hpp"
#include "resting.hpp"
#include "conditional_events.hpp"
#include "renderer.hpp"
#include "hud_data.hpp"
#include "hud_glyphs.hpp"
#include "text_capture.hpp"
#include "inventory_layout.hpp"
#include "inventory_renderer.hpp"
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
#include "menu_renderer.hpp"
#include "title_font_data.hpp"
#include "map_renderer.hpp"
namespace hk3ds {
void ui::diagnosticText(C2D_TextBuf buffer,const char* value,float x,float y,float scale)
{
    C2D_Text parsed;
    C2D_TextParse(&parsed,buffer,value);C2D_TextOptimize(&parsed);
    if constexpr(diagnosticsEnabled){
        textcapture::String capture(value);
        float width=0,height=0;C2D_TextGetDimensions(&parsed,.85f*scale,scale,&width,&height);
        textcapture::block(x,y,width,height,value);
    }
    C2D_DrawText(&parsed,C2D_WithColor,x,y,0,.85f*scale,scale,C2D_Color32(255,255,255,255));
}
float HudRenderer::textWidth(const char* value,float scale,float spacing) const
{
    return hudtext::textWidth(value,scale,spacing);
}
void HudRenderer::glyph(char value,float x,float baseline,float scale,const C2D_ImageTint* tint) const
{
    const unsigned size=hudtext::glyphScaleIndex(scale);
    if(size<promptGlyphs_.size())sprite(frame(promptGlyphs_[size][value-32]),x,baseline,scale/promptdata::lcdScales[size],tint,1,true);
}
void HudRenderer::text(const char* value,float x,float baseline,float alpha,float scale,float spacing) const
{
    textcapture::String capture(value,C2D_Color32f(1,1,1,alpha));
    x-=textWidth(value,scale,spacing)*.5f;
    C2D_ImageTint tint{};C2D_PlainImageTint(&tint,C2D_Color32f(1,1,1,alpha),0);
    bool first=true;
    for(auto p=value;*p;++p)if(*p>=32&&*p<=126){
        if(!first)x+=spacing;
        glyph(*p,x,baseline,scale,&tint);
        x+=promptdata::glyphAdvance[*p-32]*scale;
        first=false;
    }
}
// Source pages already carry dialogue timing. Reflow their hard-wrapped lines
// for the LCD without changing the source dialogue state or input rules.
namespace {
void readingText(char* out,std::size_t capacity,const char* source,unsigned visible=~0u) {
    std::size_t n=0;
    for(unsigned i=0;source&&source[i]&&source[i]!='\f'&&i<visible&&n+1<capacity;++i) {
        const char c=source[i];
        if(c=='\n') {
            if(i&&source[i-1]=='\n')out[n++]='\n';
            else out[n++]=' ';
        }else out[n++]=c;
    }
    out[n]=0;
}
}
bool HudRenderer::readingPanelVisible(const ConditionalEvents* events,const WorldQuestDialogue* quest,const Cartographer* npc,const AbilityPickup* ability,const char* hornet) const {
    // A world-quest prompt (Inspect/Listen) is the top-screen Arrow Prompt, not a lower-screen reading panel.
    return (hornet&&hornet[0])||(inspection_&&inspection_->locked())||(quest&&quest->locked())||
        (events&&events->dialogue!=EventDialogue::None)||
        (npc&&!npc->shop&&npc->locked())||
        (prompts_&&prompts_->readingPanelVisible())||
        (ability&&ability->panelVisible());
}
// Pickup text shared by the top-screen board and the lower-screen Yes/No choice.
static void abilityReadingText(const AbilityPickup& ability,char* body,std::size_t capacity) {
    body[0]=0;
    const bool item=ability.phase==AbilityPickup::Phase::Item;
    const unsigned end=item?4u:ability.phase==AbilityPickup::Phase::Dialogue?3u:2u;
    for(unsigned i=item?1u:0u;i<end;++i){
        const auto used=std::strlen(body);
        std::snprintf(body+used,capacity-used,"%s%s",used?"\n":"",ability.line(i));
    }
}
ReadingSources HudRenderer::readingSources(const ConditionalEvents* events,const WorldQuestDialogue* quest,const Cartographer* npc,const AbilityPickup* ability,const char* hornet) const {
    ReadingSources s;
    s.hornet=hornet&&hornet[0];
    s.inspect=inspection_&&inspection_->locked();
    s.listen=(quest&&quest->locked())||(events&&events->dialogue!=EventDialogue::None);
    const bool npcLocked=npc&&!npc->shop&&npc->locked();
    s.npcText=npcLocked;
    s.npcChoice=npcLocked&&npc->dialogue==Cartographer::Dialogue::Prompt;
    const bool abilityLocked=ability&&ability->panelVisible();
    s.abilityText=abilityLocked&&ability->phase!=AbilityPickup::Phase::Confirm;
    s.abilityChoice=abilityLocked&&!s.abilityText;
    s.focus=prompts_&&prompts_->tabletAlpha()>0;
    return s;
}
bool HudRenderer::lowerReadingVisible(const ConditionalEvents* events,const WorldQuestDialogue* quest,const Cartographer* npc,const AbilityPickup* ability,const char* hornet) const {
    return readingRoute(readingSources(events,quest,npc,ability,hornet)).bottom;
}
void HudRenderer::drawReadingBoard(C3D_RenderTarget* target,const ConditionalEvents* events,const WorldQuestDialogue* quest,const Cartographer* npc,const AbilityPickup* ability,const char* hornet) const {
    const auto s=readingSources(events,quest,npc,ability,hornet);
    if(!target||(!s.hornet&&!s.npcText&&!s.abilityText))return;
    C2D_SceneBegin(target);C2D_ViewReset();
    const auto c=uiCanvas();char body[2048]{};
    const char* heading="";const char* source=nullptr;
    if(s.hornet){heading="Hornet";source=hornet;}
    else if(s.npcText){heading=npc->card?"Inspect":"Cornifer";source=npc->text();}
    else {heading=ability->title();abilityReadingText(*ability,body,sizeof(body));}
    if(source)readingText(body,sizeof(body),source);
    // Same modal dimming as the lore board.
    C2D_DrawRectSolid(0,0,0,400,240,C2D_Color32(0,0,0,static_cast<u8>(255*promptlayout::modalDimOpacity)));
    const auto identity=ui::textIdentity(source?source:body);
    if(identity!=readingIdentity_){readingIdentity_=identity;readingPage_=0;}
    // Lore board geometry: original fleur frames, speaker heading, paged body.
    c.art(c.user,ui::Art::FrameTop,{64,35,192,16});
    ui::label(c,heading,160,68,true);
    const auto pages=ui::pagedText(c,body,{36,76,328,112},readingPage_);
    readingPage_=pages.page;readingPages_=pages.pages;
    // Question choices keep the lower screen; the top board shows only the question.
    if(s.npcChoice)return;
    const bool more=readingPage_+1<readingPages_||(s.npcText&&!s.hornet&&npc->hasNextPage());
    c.art(c.user,ui::Art::FrameBottom,{64,192,192,16});
    sprite(frame(more?loreArrow_:loreStop_),160,200,1.3f);
    char hint[32];std::snprintf(hint,sizeof(hint),"B / X: %s",more?"Continue":"Close");
    const float width=c.measure(c.user,hint,ui::Size::Body);
    ui::wrappedText(c,hint,{160-width/2,222,width+16,20});
}
void HudRenderer::drawReadingPanel(const ConditionalEvents* events,const WorldQuestDialogue* quest,const Cartographer* npc,const AbilityPickup* ability,const char* hornet) const {
    C2D_ViewReset();
    const auto c=uiCanvas();char body[2048]{};
    const auto s=readingSources(events,quest,npc,ability,hornet);
    if(s.npcChoice){
        // The question is on the top-screen board; the purchase choice stays on the lower screen (choice rule).
        ui::window(c,npc->card?"Inspect":"Cornifer");
        ui::listRow(c,{16,72,288,28},"Yes","30 Geo",npc->yes);
        ui::listRow(c,{16,120,288,28},"No",nullptr,!npc->yes);
        const ui::Action actions[]={{"Up/Down","Choose"},{"B / X","Confirm"}};
        ui::promptBar(c,actions,2);return;
    }
    if(s.abilityChoice){
        ui::window(c,ability->title());
        abilityReadingText(*ability,body,sizeof(body));
        ui::wrappedText(c,body,{20,52,280,128});
        const ui::Action actions[]={{"B / X","Yes / No"}};
        ui::promptBar(c,actions,1);return;
    }
    if(s.focus){
        ui::window(c);
        // Resident source ornaments are shared by every reading panel.
        sprite(frame(loreFleurTop_),160,14,.6f);
        sprite(frame(loreFleurBottom_),160,203,.6f);
        ui::wrappedText(c,"Focus",{20,24,280,24},ui::Size::Heading);
        std::snprintf(body,sizeof(body),"Collect SOUL by striking enemies. Once enough SOUL is collected, hold Y to focus SOUL and HEAL.");
        const auto identity=ui::textIdentity(body);
        if(identity!=readingIdentity_){readingIdentity_=identity;readingPage_=0;}
        const auto pages=ui::pagedText(c,body,{20,52,280,128},readingPage_);
        readingPage_=pages.page;readingPages_=pages.pages;
        if(pages.pages>1){
            char label[64];std::snprintf(label,sizeof(label),"[L / R] Read %u / %u",pages.page+1,pages.pages);
            ui::wrappedText(c,label,{20,188,280,20});
        }
    }
}void HudRenderer::turnReadingPage(int direction) {
    if(direction>0&&readingPage_+1<readingPages_)++readingPage_;
    if(direction<0&&readingPage_)--readingPage_;
}
void HudRenderer::drawAbility(const AbilityPickup& pickup,const AnimationBank& art,const HeroRenderer& renderer) const
{
    // The top-screen reading board owns the pickup title and text (drawReadingBoard).
    (void)pickup;(void)art;(void)renderer;
}
void HudRenderer::drawCartographer(const Cartographer& npc,const AnimationBank& art,const HeroRenderer& renderer) const
{
    if(!npc.locked()&&!npc.prompt)return;
    // Iselda's purchase step is the shared shop menu (drawIseldaShop), not this text.
    if(npc.shop&&npc.dialogue==Cartographer::Dialogue::Prompt)return;
    C2D_ViewReset();
    const auto line=[&](const char* start,const char* end,float baseline){
        textcapture::String capture(start);
        float width=0;for(auto cursor=start;cursor<end;++cursor)if(*cursor>=32&&*cursor<127)width+=promptdata::glyphAdvance[*cursor-32]*dialoguedata::scale;
        float left=200-width*.5f;
        for(auto cursor=start;cursor<end;++cursor)if(*cursor>=32&&*cursor<127){glyph(*cursor,left,baseline,dialoguedata::scale,nullptr);left+=promptdata::glyphAdvance[*cursor-32]*dialoguedata::scale;}
    };
    if(!npc.locked()){const char* value="Listen - Up / Down";line(value,value+std::strlen(value),202);return;}
    const auto flourish=[&](const char* name,float baseline){AnimationPlayer player;player.play(art.find(name));renderer.drawAnimation(player,art,200,baseline,16,1);};
    flourish("Dialogue Top",94);
    // Long lines wrap at word breaks to the top-screen text width instead of running off both edges.
    const auto advance=[](char c){return c>=32&&c<127?promptdata::glyphAdvance[c-32]*dialoguedata::scale:0.f;};
    const auto measure=[&](const char* from,const char* to){float w=0;for(auto p=from;p<to;++p)w+=advance(*p);return w;};
    const char* start=npc.text();float baseline=116;
    while(*start&&*start!='\f'){
        const char* end=start;while(*end&&*end!='\n'&&*end!='\f')++end;
        const char* from=start;
        do{
            const char* fit=from;const char* lastSpace=nullptr;
            while(fit<end&&measure(from,fit+1)<=340.f){if(*fit==' ')lastSpace=fit;++fit;}
            const char* to=fit>=end?end:(lastSpace&&lastSpace>from?lastSpace:fit);
            line(from,to,baseline);baseline+=15;
            from=to<end&&*to==' '?to+1:to;
        }while(from<end);
        if(*end!='\n')break;
        start=end+1;
    }
    flourish("Dialogue Bottom",195);
    flourish(npc.hasNextPage()?"Dialogue Arrow":"Dialogue Stop",196);
    const char* controls="B / X";
    if(npc.dialogue==Cartographer::Dialogue::Prompt)controls=npc.shop?(npc.yes?"> Yes - 40 Geo     No":"Yes - 40 Geo     > No"):(npc.yes?"> Yes - 30 Geo     No":"Yes - 30 Geo     > No");
    line(controls,controls+std::strlen(controls),222);
}
float MenuRenderer::textWidth(const char* label,float scale) const {
    float width=0;
    for(const char* c=label;*c;++c){const unsigned index=static_cast<unsigned char>(*c)-32;
        if(index<95)width+=(hk::menu_font::glyphs[index].advance64/64.f+.75f)*scale*1.875f;}
    return width;
}
void MenuRenderer::text(const char* label,float x,float y,float scale,bool centered,unsigned color){
    if(!label)return;
    textcapture::String capture(label,color);
    if(font_){
        const float factor=scale*1.875f;
        const float width=textWidth(label,scale);
        if(centered)x-=width*.5f;
        const auto page=C2D_SpriteSheetGetImage(font_,0);
        C2D_ImageTint tint{};C2D_PlainImageTint(&tint,color,1);
        for(const char* c=label;*c;++c){
            const unsigned index=static_cast<unsigned char>(*c)-32;if(index>=95)continue;
            const auto& g=hk::menu_font::glyphs[index];
            if(g.width&&g.height){
                Tex3DS_SubTexture sub{};sub.width=g.width;sub.height=g.height;
                sub.left=float(g.x)/page.tex->width;sub.right=float(g.x+g.width)/page.tex->width;
                sub.top=1-float(g.y)/page.tex->height;sub.bottom=1-float(g.y+g.height)/page.tex->height;
                textcapture::glyph(x+g.offsetX*factor,y+(12+g.offsetY)*factor,g.width*factor,g.height*factor);
                C2D_DrawImageAt({page.tex,&sub},std::round(x+g.offsetX*factor),std::round(y+(12+g.offsetY)*factor),0,&tint,factor,factor);
            }
            x+=(g.advance64/64.f+.75f)*factor;
        }
        return;
    }
    if(!text_)return;
    C2D_Text parsed;C2D_TextParse(&parsed,text_,label);C2D_TextOptimize(&parsed);
    if(centered){float width=0;C2D_TextGetDimensions(&parsed,scale,scale,&width,nullptr);x-=width*.5f;}
    float w=0,h=0;C2D_TextGetDimensions(&parsed,scale,scale,&w,&h);
    textcapture::block(x,y,w,h,label);
    C2D_DrawText(&parsed,C2D_WithColor,x,y,0,scale,scale,color);
}
void MapRenderer::countsText(const char* counts,ui::Canvas canvas) {
    if(canvas.text){
        // Leave two complete prompt rows clear below the map counters.
        ui::wrappedText(canvas,counts,{16,156,288,32});
    }
}

void HudRenderer::textLines(const char* value,float x,float baseline,float size,float height,bool leftAligned) const {
    char line[256]{};unsigned n=0;
    for(const char* p=value;;++p){
        if(*p=='\n'||!*p){line[n]=0;text(line,leftAligned?x+textWidth(line,size)*.5f:x,baseline,1,size);baseline+=height;n=0;if(!*p)break;}
        else if(n<sizeof(line)-1)line[n++]=*p;
    }
}
ui::Canvas HudRenderer::menuCanvas() const {
    return {const_cast<HudRenderer*>(this),
        [](void* p,const char* value,ui::Size size){
            char caps[256]{};unsigned n=0;
            for(;value[n]&&n+1<sizeof(caps);++n)caps[n]=value[n]>='a'&&value[n]<='z'?char(value[n]-'a'+'A'):value[n];
            const float scale=size==ui::Size::Compact?1.f/2.2f:1.f/1.875f;
            return static_cast<HudRenderer*>(p)->menuArt_.textWidth(caps,scale);
        },
        [](void* p,const char* value,float x,float y,ui::Size size,std::uint32_t color){
            char caps[256]{};unsigned n=0;
            for(;value[n]&&n+1<sizeof(caps);++n)caps[n]=value[n]>='a'&&value[n]<='z'?char(value[n]-'a'+'A'):value[n];
            const float scale=size==ui::Size::Compact?1.f/2.2f:1.f/1.875f;
            static_cast<HudRenderer*>(p)->menuArt_.text(caps,x,y,scale,false,color);
        },
        [](void*,ui::Rect r,std::uint32_t color){C2D_DrawRectSolid(r.x,r.y,0,r.w,r.h,color);},
        [](void* p,ui::Art art,ui::Rect r){
            const auto* self=static_cast<HudRenderer*>(p);
            if(art==ui::Art::Pointer||art==ui::Art::PointerRight){
                self->menuArt_.arrow(art==ui::Art::PointerRight,r.x,r.y,r.w,r.h);return;
            }
            const auto c=self->uiCanvas();if(c.art)c.art(c.user,art,r);
        }};
}
ui::Canvas HudRenderer::uiCanvas() const {
    return {const_cast<HudRenderer*>(this),
        [](void* p,const char* value,ui::Size size){return static_cast<HudRenderer*>(p)->textWidth(value,ui::scale(size));},
        [](void* p,const char* value,float x,float y,ui::Size size,std::uint32_t color){
            textcapture::String capture(value,color);
            const auto* self=static_cast<HudRenderer*>(p);
            const float s=ui::scale(size);
            // Preserve the source font's grayscale edge coverage.
            // Disabled choices use opacity to retain the source glyph edge coverage.
            const float opacity=float((color>>24)&0xff)/255.f * (color==0xff7a746e?0.48f:1.f);
            C2D_ImageTint tint{};C2D_PlainImageTint(&tint,C2D_Color32f(1,1,1,opacity),0);
            const float baseline=y+ui::baselineOffset(size);
            for(auto c=value;*c;++c)if(*c>=32&&*c<=126){self->glyph(*c,x,baseline,s,&tint);x+=promptdata::glyphAdvance[*c-32]*s;}
        },
        [](void*,ui::Rect r,std::uint32_t color){C2D_DrawRectSolid(r.x,r.y,0,r.w,r.h,color);},
        [](void* p,ui::Art art,ui::Rect r){
            const auto* self=static_cast<HudRenderer*>(p);
            if(art==ui::Art::Pointer&&self->inventoryArt_&&self->inventoryArt_->drawIconFit("Cursor",r))return;
            // The lore footer has a deliberate centre gap. Menus use one
            // continuous original fleur, mirrored vertically for the footer.
            const bool footer=art==ui::Art::FrameBottom;
            const auto* s=self->frame(art==ui::Art::Pointer?self->loreFleurBottom_:self->loreFleurTop_);
            if(!s||s->right<=s->left||s->top<=s->bottom)return;
            const float width=(s->right-s->left)*huddata::pixelsPerUnit,height=(s->top-s->bottom)*huddata::pixelsPerUnit;
            const float scale=std::min(r.w/width,r.h/height);
            const float anchor=footer?-s->bottom:s->top;
            self->sprite(s,r.x+(r.w-width*scale)/2-s->left*huddata::pixelsPerUnit*scale,
                r.y+(r.h-height*scale)/2+anchor*huddata::pixelsPerUnit*scale,scale,nullptr,footer?-1.f:1.f);
        }};
}
}
#endif
