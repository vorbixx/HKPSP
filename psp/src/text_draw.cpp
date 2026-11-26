#include "text_pack.hpp"
#include "text_draw.hpp"
#include "actors.hpp"
#include "tutorial.hpp"
#include "cartographer.hpp"
#include "sprite_layout.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace hkpsp {
namespace {
constexpr float promptLabelLift=15.f;

unsigned next(const char*& p){
    const unsigned c=static_cast<unsigned char>(*p++);
    if(c<128)return c;
    unsigned value=0,count=0;
    if(c>=0xc2&&c<=0xdf){value=c&31;count=1;}
    else if(c>=0xe0&&c<=0xef){value=c&15;count=2;}
    else if(c>=0xf0&&c<=0xf4){value=c&7;count=3;}
    else throw std::runtime_error("Invalid UTF-8 text");
    for(unsigned i=0;i<count;++i){const unsigned d=static_cast<unsigned char>(*p);
        if((d&0xc0)!=0x80)throw std::runtime_error("Truncated UTF-8 text");
        value=(value<<6)|(d&63);++p;
    }
    if((count==1&&value<128)||(count==2&&value<2048)||(count==3&&value<65536)||
       value>0x10ffff||(value>=0xd800&&value<=0xdfff))throw std::runtime_error("Invalid UTF-8 scalar");
    return value;
}
std::array<float,4> colour(std::uint32_t c){
    return {{float(c&255)/255,float((c>>8)&255)/255,float((c>>16)&255)/255,float(c>>24)/255}};
}
void promptArt(DrawList& list,const ActorAtlas& atlas,const hk3ds::AnimationPlayer& player,
               ActorPose pose,hk3ds::Vec2 camera){
    const auto* s=player.current(atlas.bank);if(!s)return;
    const float ppu=hk3ds::roomFocalLength(screenWidth,screenHeight)/(38.1f+pose.depth);
    const auto layout=hk3ds::spriteLayout(*s,0,0,ppu,pose.scale.x,pose.scale.y);
    // Inherited quarter-resolution prompt art must stay within native texels.
    // Cap uniformly around the authored anchor, preserving each part's aspect.
    const float limit=std::max({1.f,std::abs(layout.scaleX),std::abs(layout.scaleY)});
    pose.scale.x/=limit;pose.scale.y/=limit;
    appendActor(list,atlas.bank,player,pose,camera,atlas.page);
}

}
unsigned TextCanvas::font(hk3ds::ui::Size size){
    return size==hk3ds::ui::Size::Body?0:size==hk3ds::ui::Size::Compact?1:
        size==hk3ds::ui::Size::Heading?2:3;
}
TextCanvas::TextCanvas(DrawList& target,const HudArt& source):
    TextCanvas(target,source.fonts,source.fontPage){art=&source;}
TextCanvas::TextCanvas(DrawList& target,const FontBank& source,unsigned firstPage):
    list(target),fonts(source),fontPage(firstPage){
    constexpr unsigned sizes[]={15,11,14,18};
    for(unsigned i=0;i<4;++i)if(fonts.fonts.at(i).size!=sizes[i]||
        fonts.fonts[i].capitals<(i<2?9:11))throw std::runtime_error("Text font size contract");
    // Borrow an existing white texel. No new texture or backend fill pass is needed.
    if(!list.textures)throw std::runtime_error("Text Canvas needs resident font pages");
    for(unsigned page=0;page<fonts.pageCount;++page){
        const auto& t=list.textures->at(fontPage+page);
        for(unsigned i=0;i<t.indices.size();++i)
            if(t.paletteFor(0)[t.indices[i]]==std::array<unsigned char,4>{{255,255,255,255}}){
                solidPage=fontPage+page;solid={float(i%t.width)+.5f,float(i/t.width)+.5f};return;
            }
    }
    throw std::runtime_error("Font atlas lacks an opaque white fill texel");
}
float TextCanvas::measure(const char* value,hk3ds::ui::Size size)const{
    // Every text sink accepts "@key" text from the text pack.
    value=hk3ds::text::resolve(value);
    if(!value)return 0;
    const unsigned f=font(size);float width=0;unsigned previous=0;
    for(auto p=value;*p;){const auto cp=next(p);
        if(previous)width+=fonts.kern(f,previous,cp);
        width+=fonts.glyph(f,cp).advance;previous=cp;
    }
    return width;
}
void TextCanvas::text(const char* value,float x,float y,hk3ds::ui::Size size,std::uint32_t color){
    value=hk3ds::text::resolve(value);
    if(!value||!(color>>24))return;
    const auto f=font(size);const float baseline=y+hk3ds::ui::baselineOffset(size);
    unsigned previous=0;
    for(auto p=value;*p;){const auto cp=next(p);const auto& g=fonts.glyph(f,cp);
        if(previous)x+=fonts.kern(f,previous,cp);
        if(g.width&&g.height){
            const float px=std::round(x+g.bearingX),py=std::round(baseline+g.bearingY);
            Quad q;q.page=fontPage+g.page;q.u0=float(g.x);q.v0=float(g.y);
            q.u1=float(g.x+g.width);q.v1=float(g.y+g.height);q.rgba=colour(color);
            q.positions={{{px,py},{px+g.width,py},{px+g.width,py+g.height},{px,py+g.height}}};
            list.quads.push_back(q);
        }
        x+=g.advance;previous=cp;
    }
}
void TextCanvas::fill(hk3ds::ui::Rect r,std::uint32_t color){
    if(r.w<=0||r.h<=0||!(color>>24))return;
    Quad q;q.page=solidPage;q.u0=q.u1=solid.x;q.v0=q.v1=solid.y;q.rgba=colour(color);
    q.positions={{{r.x,r.y},{r.x+r.w,r.y},{r.x+r.w,r.y+r.h},{r.x,r.y+r.h}}};list.quads.push_back(q);
}
hk3ds::ui::Canvas TextCanvas::canvas(){
    return {this,
        [](void* p,const char* v,hk3ds::ui::Size s){return static_cast<TextCanvas*>(p)->measure(v,s);},
        [](void* p,const char* v,float x,float y,hk3ds::ui::Size s,std::uint32_t c){static_cast<TextCanvas*>(p)->text(v,x,y,s,c);},
        [](void* p,hk3ds::ui::Rect r,std::uint32_t c){static_cast<TextCanvas*>(p)->fill(r,c);}};
}
void TextCanvas::prepareLore(hk3ds::LoreTabletBank& bank){
    auto measure=canvas();measure.text=[](void*,const char*,float,float,hk3ds::ui::Size,std::uint32_t){};
    for(auto& tablet:bank.tablets){
        decltype(tablet.pages) pages{};unsigned count=0;
        for(unsigned source=0;source<tablet.pageCount;++source){
            auto normalized=tablet.pages[source];
            // Single newlines were imposed for the 3DS width. Retain paragraphs
            // and source page boundaries, then wrap at the native font metrics.
            for(unsigned i=0;normalized[i];++i)
                if(normalized[i]=='\n'&&(i==0||tablet.pages[source][i-1]!='\n')&&normalized[i+1]!='\n')
                    normalized[i]=' ';
            std::size_t offset=0;
            while(normalized[offset]){
                // Paragraph spacing at a page break must not consume a native row.
                while(normalized[offset]==' '||normalized[offset]=='\n')++offset;
                if(!normalized[offset])break;
                const auto result=hk3ds::ui::wrappedText(measure,normalized.data()+offset,loreTextRect);
                if(!result.consumed||count==pages.size()||result.consumed>=pages[count].size())
                    throw std::runtime_error("Native lore pagination capacity");
                std::memcpy(pages[count++].data(),normalized.data()+offset,result.consumed);
                offset+=result.consumed;
            }
        }
        tablet.pages=pages;tablet.pageCount=count;
    }
}
void TextCanvas::sprite(const char* name,Point anchor,float sx,float sy,float alpha,const AnimationAtlas* atlas,float seconds){
        if(!atlas&&!art)throw std::runtime_error("Text artwork needs the HUD atlas");
        const auto& bank=atlas?atlas->bank:art->bank;
        const auto* clip=bank.find(name);
        if(!clip||!clip->count)throw std::runtime_error("Missing native text artwork");
        const auto frame=std::min(std::uint32_t(seconds*clip->fps),clip->count-1);
        const auto* s=&bank.sprites.at(bank.frames.at(clip->first+frame));
        const auto layout=hk3ds::spriteLayout(*s,anchor.x,anchor.y,hudPixelsPerUnit,sx,sy);
        if(layout.scaleX>1||layout.scaleY>1)throw std::runtime_error("Upscaled native text artwork");
        Quad q;q.page=(atlas?atlas->page:art->firstPage)+s->page;q.clut=s->clut;
        q.u0=float(s->x);q.v0=float(s->y);q.u1=float(s->x+s->width);q.v1=float(s->y+s->height);
        q.rgba[3]=alpha;
        const float x=std::round(layout.x),y=std::round(layout.y);
        const float w=s->width*layout.scaleX,h=s->height*layout.scaleY;
        q.positions={{{x,y},{x+w,y},{x+w,y+h},{x,y+h}}};list.quads.push_back(q);
}
void TextCanvas::lore(const hk3ds::LoreInspection& inspection){
    const float alpha=inspection.alpha();if(alpha<=0)return;
    // The shared inspection fade darkens the room behind lore; the original does not go fully black.
    // Settled backdrop [M 2026-11-19]: the room keeps 0.40 of its brightness (focus-board-before.png
    // against reminder-interact-before.png, three clean background regions 0.38-0.41), so the black
    // fill reaches 0.60 at full fade. A solid fill needs no enlarged bitmap or additional atlas page.
    constexpr float loreBackdropAlpha=.6f;
    fill({0,0,screenWidth,screenHeight},unsigned(std::clamp(alpha*loreBackdropAlpha,0.f,1.f)*255)<<24);
    // Original settled DialogueBox: resources:9786; child flourishes 9688/9982.
    const auto ornament=[&](const char* name,Point anchor,float scale){sprite(name,anchor,scale,scale,alpha);};
    ornament("Lore Fleur Top",{240,136-(4.51f+.75f*2.85f)*hudPixelsPerUnit},.75f);
    ornament("Lore Fleur Bottom",{240,136-(4.51f-.75f*2.73f)*hudPixelsPerUnit},.75f);
    if(inspection.phase()==hk3ds::InspectionPhase::Reading){
        const auto tint=(unsigned(std::clamp(alpha,0.f,1.f)*255)<<24)|0xffffff;
        const auto result=hk3ds::ui::readingText(canvas(),inspection.text(),loreTextRect,
                                              inspection.visibleCharacters(),tint);
        if(result.overflow)throw std::runtime_error("Native lore page overflow");
        if(inspection.pageComplete())
            ornament(inspection.hasNextPage()?"Lore Arrow":"Lore Stop",
                     {240,136-(4.49f-2.795f)*hudPixelsPerUnit},1.3f);
    }
}
void TextCanvas::areaTitle(const hk3ds::AreaTitle& title){
    const float alpha=title.alpha();
    if(title.originalRegionLayout()){
        using Title=hk3ds::AreaTitle;
        const unsigned region=title.areaId()-1;
        const float time=title.fleurTime();
        if(time<Title::fleursUp&&alpha<=0)return;
        if(!regionTitleArt||!regionTitleArt->bank.pageCount)throw std::runtime_error("Missing region title atlas");
        if(time>=Title::fleursUp){
            const bool down=time>=Title::titlesDown;
            const float elapsed=time-(down?Title::titlesDown:Title::fleursUp);
            sprite(down?"Region Fleur Top Down":"Region Fleur Top",{},1,1,1,regionTitleArt,elapsed);
            sprite(down?"Region Fleur Bottom Down":"Region Fleur Bottom",{0,region==2?-13.f:0.f},1,1,1,regionTitleArt,elapsed);
        }
        if(alpha>0){
            constexpr const char* layers[2][3][2]={
                {{"DIRTMOUTH Small","DIRTMOUTH Small Extra"},{"CROSSROADS Small","CROSSROADS Small Extra"},{"GREENPATH Small",nullptr}},
                {{"DIRTMOUTH Large","DIRTMOUTH Large Extra"},{"CROSSROADS Large","CROSSROADS Large Extra"},{"GREENPATH Large",nullptr}}};
            for(const auto* name:layers[title.large()][region])if(name)sprite(name,{},1,1,alpha,regionTitleArt);
        }
        return;
    }
    if(alpha<=0)return;
    // Tutorial_01 KINGSPASS: resources:11294/11184/19250, Visited small title.
    // The original Trajan capitals project to ~13 px; resident Trajan18 has 14.
    // Other regions' large-title compositions belong to their room milestones.
    // Region names (3DS hud_renderer.cpp:526-528): centred at the bottom. The 3DS top screen is
    // 400x240 with baseline y=215; scaled by the PSP's uniform 272/240 that is baseline 244. This
    // placement is [H]: no original capture of the region label exists in HK3DS _private/research.
    if(const char* label=title.regionLabel()){
        constexpr auto size=hk3ds::ui::Size::Title;
        constexpr float regionBaseline=215*272.f/240.f;
        const auto regionTint=(unsigned(std::clamp(alpha,0.f,1.f)*255)<<24)|0xffffff;
        const auto c=canvas();
        c.text(c.user,label,240-measure(label,size)*.5f,regionBaseline-hk3ds::ui::baselineOffset(size),
               size,regionTint);
        return;
    }
    if(title.artPart())return;
    constexpr float x=240-13.68f*hudPixelsPerUnit;
    constexpr float midline=-17.5f*(36.f/70.f)*.1f*.349259f;
    constexpr float baseline=136-(-13.15f+6.1f+midline)*hudPixelsPerUnit;
    const auto tint=(unsigned(std::clamp(alpha,0.f,1.f)*255)<<24)|0xffffff;
    const auto c=canvas();
    c.text(c.user,"King\xe2\x80\x99s Pass",x,baseline-hk3ds::ui::baselineOffset(hk3ds::ui::Size::Title),
           hk3ds::ui::Size::Title,tint);
}
void TextCanvas::bossTitle(const char* name,float alpha){
    name=hk3ds::text::resolve(name);
    if(!(alpha>0))return;
    // Original fight-start frame (480x270, boss-fk look-sweep/original/fk-fight-start-w0): "False Knight"
    // ink x284-440, caps y237-248 (baseline 249). Trajan Title (18 px, caps 14) at the original left edge
    // and baseline; the original letter-spacing is wider (157 px against 135 px), accepted as native.
    constexpr float x=284,baseline=249;
    const auto tint=(unsigned(std::clamp(alpha,0.f,1.f)*255)<<24)|0xffffff;
    const auto c=canvas();
    c.text(c.user,name,x,baseline-hk3ds::ui::baselineOffset(hk3ds::ui::Size::Title),
           hk3ds::ui::Size::Title,tint);
}
void TextCanvas::dialoguePage(const char* source,bool pageEnd,bool more,const ActorArt& actors){
    source=hk3ds::text::resolve(source);
    // The one original NPC dialogue box (ornaments, native Perpetua body, page arrow or stop).
    // Reflow inherited LCD line breaks; a form feed ends the visible page when pageEnd is set.
    std::array<char,768> body{};
    for(unsigned i=0;i+1<body.size()&&source[i]&&!(pageEnd&&source[i]=='\f');++i){
        const bool breakLine=source[i]=='\n'&&(i==0||source[i-1]!='\n')&&source[i+1]!='\n'&&!(pageEnd&&source[i+1]=='\f');
        body[i]=breakLine?' ':source[i];
    }
    constexpr hk3ds::ui::Rect panel{87,48,306,87};
    constexpr hk3ds::ui::Rect textRect{99,56,282,180};
    constexpr float ornamentGap=10,markerGap=7;
    const auto panelIndex=list.quads.size();
    fill(panel,0x99000000u);
    sprite("Dialogue Top",{240,47},1,1,1,&actors.events);
    const auto result=hk3ds::ui::wrappedText(canvas(),body.data(),textRect,hk3ds::ui::Size::Body,0xffffffffu);
    if(result.overflow)throw std::runtime_error("Native dialogue page overflow");
    const float textBottom=textRect.y+result.lines*hk3ds::ui::lineHeight(hk3ds::ui::Size::Body);
    const float bottomY=textBottom+ornamentGap;
    const float panelBottom=std::max(panel.y+panel.h,bottomY+markerGap+2);
    list.quads[panelIndex].positions[2].y=list.quads[panelIndex].positions[3].y=panelBottom;
    sprite("Dialogue Bottom",{240,bottomY},1,1,1,&actors.events);
    sprite(more?"Dialogue Arrow":"Dialogue Stop",{240,bottomY+markerGap},1.25f,1.25f,1,&actors.events);
}
void TextCanvas::dialogue(const hk3ds::ConditionalEvents& events,const ActorArt& actors){
    if(events.dialogue==hk3ds::EventDialogue::None)return;
    dialoguePage(events.text(),false,events.hasNextPage(),actors);
}
void TextCanvas::cartographer(const hk3ds::Cartographer& npc,unsigned geo,const ActorArt& actors){
    // Cornifer talks in the same original dialogue box as NPC events: no speaker heading and no
    // button hints. Iselda's purchase step is the shop screen (appendText), but her talk and the
    // purchase result use this box. The purchase question uses the original
    // Yes/No box (M7.4). text() still holds later pages after the form feed, so only the visible
    // page is drawn.
    if(!npc.locked()||(npc.shop&&npc.dialogue==hk3ds::Cartographer::Dialogue::Prompt))return;
    if(npc.dialogue==hk3ds::Cartographer::Dialogue::Prompt){
        // ui-compare yes-no-map: the original shows "Not enough Geo." under the choices when unaffordable.
        const unsigned price=hk3ds::CartographySave::cornifierPrice;
        choiceBox(npc.text(),price,npc.yes,actors,nullptr,geo>=price);
        return;
    }
    dialoguePage(npc.text(),true,npc.hasNextPage(),actors);
}
void TextCanvas::greenpathIntro(const hk3ds::GreenpathEncounter& encounter,const ActorArt& actors){
    if(!encounter.introActive())return;
    const auto page=encounter.introPage();
    dialoguePage(encounter.introText(page),false,page<3,actors);
}
void TextCanvas::choiceBox(const char* question,unsigned price,bool yes,const ActorArt& actors,
                           const char* info,bool affordable,bool showPrice,bool showChoices,bool wrapQuestion,
                           const char* yesLabel,const char* noLabel){
    question=hk3ds::text::resolve(question);info=hk3ds::text::resolve(info);
    // Original Yes/No box, yn-original frame-210 (480x270 px, README in build/psp/compare/yn-original):
    // top ornament x168-311 y51-63, question centred with caps y68-77, Geo coin x215-225 y95-104 and
    // the price beside it (baseline 105), "Yes" x150 and "No" x289 (baseline 125), wings x133/x205 at
    // y114 for the selected Yes, bottom ornament x191-287 y143-151. Trajan 11 for the choices and the
    // price digits (fonts.md menu labels), Perpetua 15 for the question (dialogue).
    constexpr auto body=hk3ds::ui::Size::Body,small=hk3ds::ui::Size::Compact;
    constexpr std::uint32_t white=0xffffffffu;
    // Ornaments: the native sprites are ~1.165x narrower than the original (143/122, 96/82.5 [M]);
    // the width scale stays under one texel per pixel. Anchors place the measured ink boxes.
    constexpr float ornamentScale=1.165f,topAnchorY=57,bottomAnchorY=147;
    // Backing (estimated [H], see State): dark translucent panel behind the box and a light bar behind
    // the selected choice. Both are solid quads with alpha, drawn before any text or ornament.
    fill({150,46,180,114},0x80000000u);
    sprite("Dialogue Top",{240,topAnchorY},ornamentScale,1,1,&actors.events);
    if(!wrapQuestion){
        text(question,240-measure(question,body)*.5f,77-hk3ds::ui::baselineOffset(body),body,white);
    }else{
        // Re-wrap (never shrink): greedy lines of at most questionWrapWidth px, as the 3DS choiceDialog
        // does, centred on the one-line baseline 77. Lines are measured from the source text only.
        const float questionWrapWidth=showPrice?168.f:282.f;
        constexpr std::size_t lineCapacity=96;
        // Returns the source offset after the line that starts at `offset`, filling line with its text.
        const auto nextLine=[&](std::size_t offset,char* line)->std::size_t{
            std::size_t length=0,lastSpace=0;
            line[0]=0;
            while(question[offset+length]&&question[offset+length]!='\n'&&length+1<lineCapacity){
                line[length]=question[offset+length];line[length+1]=0;
                if(measure(line,body)>questionWrapWidth){line[length]=0;break;}
                if(line[length]==' ')lastSpace=length;
                ++length;
            }
            if(question[offset+length]&&question[offset+length]!='\n'&&lastSpace)length=lastSpace;
            if(length==0&&question[offset]!='\n')length=1; // one oversized glyph: advance rather than loop forever
            line[length]=0;
            std::size_t next=offset+length;
            while(question[next]==' ')++next;
            if(question[next]=='\n')++next;
            return next;
        };
        char line[lineCapacity]{};
        std::size_t lines=0;
        for(std::size_t offset=0;question[offset];++lines)offset=nextLine(offset,line);
        // Two lines fit between the top ornament (ink to y63) and the Geo coin (from y95): baselines 76 and 90.
        constexpr float lineStep=14;
        float baseline=lines>1?76:77;
        std::size_t offset=0;
        for(std::size_t i=0;i<lines;++i){
            offset=nextLine(offset,line);
            text(line,240-measure(line,body)*.5f,baseline-hk3ds::ui::baselineOffset(body),body,white);
            baseline+=lineStep;
        }
    }
    if(showPrice){
        sprite("Coin Idle",{221,101},1,1,1);
        char digits[12];std::snprintf(digits,sizeof digits,"%u",price);
        text(digits,233,105-hk3ds::ui::baselineOffset(small),small,white);
    }
    if(showChoices){
        const char* labels[]={yesLabel,noLabel};
        // Original choice centres (ink Yes x150-184, No x289-304 [M]). Each label is centred there at its
        // drawn width, and the selection wings hug the drawn label with the original gaps (17 px left, 21 px right).
        // An unaffordable Yes is greyed (ChoiceDialog, ui.cpp).
        constexpr std::uint32_t grey=0xff7a746e;
        const float centres[]={167,296.5f};
        for(unsigned choice=0;choice<2;++choice){
            const float width=measure(labels[choice],small);
            const float left=centres[choice]-width*.5f;
            const bool active=(choice==0)==yes;
            if(active){
                fill({left-1,116,width+22,12},0x59ffffffu);
                if(hasMenuArrowPage){
                    appendMenuArrow(list,menuArrowPage,left-17,114,false);
                    appendMenuArrow(list,menuArrowPage,left+width+21,114,true);
                }
            }
            const std::uint32_t color=(choice==0&&!affordable)?grey:white;
            text(labels[choice],left,125-hk3ds::ui::baselineOffset(small),small,color);
        }
    }
    // Info line under the choices (ChoiceDialog info; "Not enough Geo." when unaffordable).
    const char* shown=affordable?info:"Not enough Geo.";
    if(shown&&*shown){
        constexpr std::uint32_t secondary=0xffd8c9bfu;
        text(shown,240-measure(shown,small)*.5f,138-hk3ds::ui::baselineOffset(small),small,secondary);
    }
    sprite("Dialogue Bottom",{240,bottomAnchorY},ornamentScale,1,1,&actors.events);
}
void TextCanvas::routeToll(const hk3ds::TollMenu& menu,unsigned geo,const ActorArt& actors){
    // HudRenderer::drawRouteToll (HK3DS src/hud_renderer.cpp:840-855): only while the menu is open.
    if(!menu.open)return;
    if(menu.crest){
        // Crest toll: its own question, no price, and no choices while refusing.
        choiceBox(menu.question?menu.question:"",0,menu.yes,actors,nullptr,true,false,!menu.refusal,true);
        return;
    }
    // Original layout (yn-original frame-210): the question stays on one line and there is no Geo balance
    // line, unlike the 3DS. Only a result message or "Not enough Geo." appears under the choices.
    // The machine's own original question (route_tolls.hpp): lift, bench (Greenpath) or gate.
    choiceBox(menu.question?menu.question:"",
              menu.price,menu.yes,actors,menu.message[0]?menu.message:nullptr,geo>=menu.price);
}
void TextCanvas::stagToll(const hk3ds::Resting& rest,unsigned geo,const ActorArt& actors){
    const auto stag=rest.stagPhase;
    if(stag!=hk3ds::StagPhase::Toll&&stag!=hk3ds::StagPhase::Poor)return;
    choiceBox(hk3ds::text::resolve("@Prompts/TOLLBOOTH_STAG#2"),rest.station.price,rest.yes,actors,nullptr,
              geo>=rest.station.price&&stag!=hk3ds::StagPhase::Poor);
}
void TextCanvas::stagDialogue(const hk3ds::Resting& rest,const ActorArt& actors){
    dialoguePage(rest.stationText(),false,rest.stationHasNextPage(),actors);
}
void TextCanvas::stagTravel(const hk3ds::Resting& rest,const ActorArt& actors){
    using hk3ds::ui::Size;
    constexpr unsigned visibleRows=8;
    constexpr float center=240, listCenter=126, rowStep=20, top=49;
    const unsigned destinations=rest.destinations();
    const unsigned opened=destinations|(1u<<rest.station.station);
    std::array<unsigned,hk3ds::stagStationCount> stations{};
    unsigned count=0,selectedRow=0;
    for(unsigned index=0;index<hk3ds::stagStationCount;++index){
        if(!(opened&(1u<<index)))continue;
        if(index==rest.selection)selectedRow=count;
        stations[count++]=index;
    }
    const unsigned first=count>visibleRows?std::min(selectedRow,count-visibleRows):0;
    const unsigned rows=std::min(visibleRows,count-first);
    fill({0,0,480,231},0xe6000000u);
    sprite("Dialogue Top",{center,29},1,1,1,&actors.events);
    text("Stations",listCenter-measure("Stations",Size::Heading)*.5f,27,Size::Heading,0xffffffffu);
    for(unsigned row=0;row<rows;++row){
        const unsigned index=stations[first+row];
        const bool selected=index==rest.selection&&(destinations&(1u<<index));
        const char* name=hk3ds::stagStations[index].label;
        const float width=measure(name,Size::Body),left=listCenter-width*.5f,y=top+row*rowStep;
        text(name,left,y,Size::Body,selected?0xffffffffu:index==rest.station.station?0xff898989u:0xffccccccu);
        if(selected&&hasMenuArrowPage){
            appendMenuArrow(list,menuArrowPage,left-20,y,false);
            appendMenuArrow(list,menuArrowPage,left+width+4,y,true);
        }
    }
    text("Travel  Square",357,188,Size::Compact,destinations?0xffffffffu:0xff898989u);
    text("Cancel  Cross",357,205,Size::Compact,0xffffffffu);
    sprite("Dialogue Bottom",{center,232},1,1,1,&actors.events);
}
void TextCanvas::reminder(const hk3ds::TutorialPrompts& prompts){
    const float alpha=prompts.alpha();if(alpha<=0)return;
    using Reminder=hk3ds::TutorialReminder;
    const auto kind=prompts.reminder();if(kind==Reminder::None)return;
    // Control Reminder resources:9446/19023: same single-screen row at .25.
    // [M] 2026-11-19 against reminder-*-before.png: the original button centre sits at y~244.5 and its
    // label baseline at y~245 (frame px); the PSP button was 17 px and the label 9 px too high. The button
    // (and backboard) moves down 17 px; the label keeps its own offset from the button (see below).
    constexpr float y=136+6.99f*hudPixelsPerUnit+17.f;
    // The original board is only 126 texels wide. Preserve source density.
    // Round the native-pixel cap down: float division/multiplication can
    // otherwise project 126.000008 pixels and violate the strict density guard.
    const float boardX=std::nextafter(126.f/(3.75f*hudPixelsPerUnit),0.f);
    // The board is 31.9 px tall; centred on the button row its bottom edge reaches 260.9, past the 260 px
    // bound of the reminder contract (test_text_runtime.py). Its centre sits 1 px higher to stay inside.
    const auto tint=(unsigned(std::clamp(alpha,0.f,1.f)*255)<<24)|0xffffff;
    const auto c=canvas();const auto size=hk3ds::ui::Size::Body;
    const auto compact=hk3ds::ui::Size::Compact;
    // The original Interact reminder has no HOLD (reminder-interact-before.png); Jump does.
    const bool hold=prompts.hold()||kind==Reminder::Jump;
    const float width=c.measure(c.user,prompts.text(),size);
    // HOLD sits on the button row, left of the button (orchestrator 2026-11-19): 9 px HOLD above the button
    // covers the hero, which stands over the button in the original.
    const float holdWidth=hold?c.measure(c.user,"HOLD",compact):0.f;
    const float holdGap=hold?6.f:0.f;
    const float groupLeft=240-(holdWidth+holdGap+18+8+width)/2;
    const float left=groupLeft+holdWidth+holdGap,iconX=left+9;
    // User 2026-11-21 (PSP-3000 photo): the shadow did not cover "Focus". The board is a soft 126-texel blob and
    // may not be stretched (one texel per pixel), but our row (HOLD on the row, text >= 9 px) is wider than the
    // original's. Cover the whole row with overlapping copies of the unscaled board.
    {
        constexpr float boardPixels=126.f,margin=24.f;
        const float span=holdWidth+holdGap+26+width+2*margin;
        const float centre=240+.02f*hudPixelsPerUnit;
        const unsigned copies=span<=boardPixels?1u:unsigned(std::ceil((span-boardPixels)/(boardPixels*.5f)))+1u;
        for(unsigned i=0;i<copies;++i){
            const float x=copies==1?centre:centre-(span-boardPixels)*.5f+(span-boardPixels)*float(i)/float(copies-1);
            sprite("Prompt Backboard",{x,y-1.f},boardX,.61532986f,alpha);
        }
    }
    // Label top y-13 puts its baseline at the original's y~245; HOLD shares that baseline.
    const float labelTop=y-13;
    c.text(c.user,prompts.text(),left+26,labelTop,size,tint);
    if(hold)c.text(c.user,"HOLD",groupLeft,labelTop+hk3ds::ui::baselineOffset(size)-hk3ds::ui::baselineOffset(compact),
                   compact,tint);
    if(kind==Reminder::Focus)sprite("Prompt Button",{iconX,y},1,1,alpha);
    else{
        const float radius=kind==Reminder::Attack?5:8;
        for(const auto edge:{hk3ds::ui::Rect{iconX-radius,y-radius,2*radius,1},
                                 {iconX-radius,y+radius-1,2*radius,1},
                                 {iconX-radius,y-radius+1,1,2*radius-2},
                                 {iconX+radius-1,y-radius+1,1,2*radius-2}})fill(edge,tint);
        if(kind!=Reminder::Attack){
            const char* label=kind==Reminder::Jump?"X":"^";
            const auto iconSize=hk3ds::ui::Size::Compact;
            c.text(c.user,label,iconX-c.measure(c.user,label,iconSize)/2,y-6,iconSize,tint);
        }
    }
}
void TextCanvas::focus(const hk3ds::TutorialPrompts& prompts){
    const float alpha=prompts.tabletAlpha();if(alpha<=0)return;
    // Original Focus_prompt_temp: level6:4395 under 5447. Keep its native
    // illustration transform; space the source text for the approved 9 px caps.
    constexpr float sx=1.2067969f*1.3119773f,sy=1.2538149f*1.262778f;
    constexpr float rootY=34.66f-28.864931f*1.262778f;
    const Point anchor{240,136-rootY*hudPixelsPerUnit};
    sprite("Focus Panel",anchor,sx,sy,alpha);
    const auto tint=(unsigned(std::clamp(alpha,0.f,1.f)*255)<<24)|0xffffff;
    const auto c=canvas();
    const auto line=[&](const char* value,float top,hk3ds::ui::Size size){
        const float width=c.measure(c.user,value,size);
        c.text(c.user,value,240-width/2,top,size,tint);
    };
    line(hk3ds::text::resolve("@Prompts/PROMPT_FOCUS_EXPLAINER1A"),164,hk3ds::ui::Size::Body);
    line(hk3ds::text::resolve("@Prompts/PROMPT_FOCUS_EXPLAINER1B#2"),184,hk3ds::ui::Size::Body);
    // PSP Focus is the right face button (Circle), represented by the source ring.
    const float holdWidth=c.measure(c.user,"HOLD",hk3ds::ui::Size::Compact);
    c.text(c.user,"HOLD",240-(holdWidth+30)/2,208,hk3ds::ui::Size::Compact,tint);
    sprite("Prompt Button",{240+(holdWidth+10)/2,217},1,1,alpha);
    line("to focus your SOUL and HEAL.",232,hk3ds::ui::Size::Body);
}
static bool questDialogueBoxOpen(const hk3ds::TutorialSession& game){
    return game.worldQuestDialogue.visible()&&
           (game.cityCrest.controlsHero()||
            (game.worldQuests.present()&&game.worldQuests.definition().event==hk3ds::WorldQuestEvent::Elderbug));
}
bool dialogueBoxOpen(const hk3ds::TutorialSession& game){
    // Same gates as appendText's dialogue, cartographer (Cornifer, purchase prompt included) and Hornet intro draws.
    // Iselda's purchase step is the shop screen, not a dialogue box (it keeps the HUD, as Sly's shop does).
    const auto& npc=game.cartographer;
    return (!game.sceneActive()&&game.conditionalEvents.dialogue!=hk3ds::EventDialogue::None)||
           (npc.locked()&&!(npc.shop&&npc.dialogue==hk3ds::Cartographer::Dialogue::Prompt))||
           game.greenpath.introActive()||questDialogueBoxOpen(game)||
           game.resting.stagPhase==hk3ds::StagPhase::Dialogue||game.resting.stagPhase==hk3ds::StagPhase::Menu;
}
void appendText(DrawList& list,TextCanvas& text,const ActorArt& actors,
                const hk3ds::TutorialSession& game,hk3ds::Vec2 camera){
    if(game.slyShop.open){text.slyShop(game);return;}
    if(game.cartographer.shop&&game.cartographer.dialogue==hk3ds::Cartographer::Dialogue::Prompt){
        text.iseldaShop(game.cartographer,game.geo);
        return;
    }
    const auto c=text.canvas();
    // main.cpp:976-1011: shared simulation owns visibility, fade and animation age.
    const float ppu=hk3ds::roomFocalLength(screenWidth,screenHeight)/38.1f;
    const auto prompt=[&](const hk3ds::PromptMarkerDraw& p,float lift){
        if(!p.visible)return;
        auto position=p.gate.promptPosition;position.y+=lift;
        const auto part=[&](const char* name,float alpha,float age){
            hk3ds::AnimationPlayer player;player.play(actors.fury.bank.find(name));player.advance(age);
            ActorPose pose;pose.position=position;pose.alpha=alpha;
            promptArt(list,actors.fury,player,pose,camera);
        };
        part("Inspect Shadow",p.alpha,0);part(p.down?"Inspect Down":"Inspect Up",1,p.age);
        char label[64]{};
        for(unsigned i=0;i+1<sizeof(label)&&p.gate.prompt[i];++i){
            const unsigned ch=static_cast<unsigned char>(p.gate.prompt[i]);
            label[i]=char(ch>='a'&&ch<='z'?ch-'a'+'A':ch);
        }
        const float width=c.measure(c.user,label,hk3ds::ui::Size::Compact);
        c.text(c.user,label,screenWidth/2+(position.x-camera.x)*ppu-width/2,
            screenHeight/2-(position.y-camera.y)*ppu-promptLabelLift,hk3ds::ui::Size::Compact,
            (unsigned(std::clamp(p.alpha,0.f,1.f)*255)<<24)|0xffffff);
    };
    for(const auto& old:game.doorPromptPresentation.fading)prompt(old.draw,0);
    prompt(game.doorPromptPresentation,0);
    const auto& arrow=game.arrowPrompt.presentation();
    prompt(arrow,arrow.authoredAnchor?0:52.f/ppu);
    const auto pickup=game.furyChest.pickup();
    const auto overlay=hk3ds::hudOverlay(pickup==hk3ds::FuryPickupPhase::Display||
        pickup==hk3ds::FuryPickupPhase::WaitButton,false,game.prompts.tabletAlpha(),game.prompts.alpha());
    // main.cpp:909-917: lore/Fury share the authored Arrow Prompt position,
    // animation and label fade. Use resident native glyphs for its label.
    const auto& inspect=game.nailInteractions.prompt();
    // HK3DS main.cpp:907-916 draws this prompt on its own visibility only, with no lore or overlay gate;
    // the PSP mirrors that so INSPECT shows over the tablet during the Interact reminder (original
    // reminder-interact-before.png). Focus instructions and the modal Item UI still hide it, as the PSP
    // overlay-priority contract requires (test_text_runtime.py). The shared hudLoreVisible rule is unchanged.
    // tutorial_render_state.cpp:130 clears nailInteractions in scenes: King's Pass prompt positions only.
    if(!game.sceneActive()&&inspect.visible&&overlay!=hk3ds::HudOverlay::Focus&&overlay!=hk3ds::HudOverlay::Item){
        ActorPose pose;pose.position=inspect.position;pose.scale=inspect.scale;pose.depth=inspect.depth;
        hk3ds::AnimationPlayer shadow;shadow.play(actors.fury.bank.require("Inspect Shadow"));
        pose.alpha=game.nailInteractions.labelAlpha();
        promptArt(list,actors.fury,shadow,pose,camera);
        pose.alpha=1;
        promptArt(list,actors.fury,inspect.player,pose,camera);
        constexpr auto size=hk3ds::ui::Size::Compact;
        const float width=c.measure(c.user,"INSPECT",size);
        c.text(c.user,"INSPECT",screenWidth/2+(inspect.position.x-camera.x)*ppu-width/2,
            screenHeight/2-(inspect.position.y-camera.y)*ppu-promptLabelLift,size,
            (unsigned(std::clamp(game.nailInteractions.labelAlpha(),0.f,1.f)*255)<<24)|0xffffff);
    }
    if(hk3ds::hudLoreVisible(overlay,game.inspection.locked()))text.lore(game.inspection);
    if(overlay==hk3ds::HudOverlay::Focus)text.focus(game.prompts);
    if(overlay==hk3ds::HudOverlay::Reminder)text.reminder(game.prompts);
    // main.cpp:953-957: Crossroads boss title, else Hornet title, else the area title.
    const auto title=selectTitle(game.crossroadsBoss.active(),
        game.crossroadsBoss.phase==hk3ds::BossPhase::Dormant,game.inspection.locked(),
        game.greenpath.titleAlpha());
    if(title==TitleKind::Boss)text.bossTitle("False Knight",game.crossroadsBoss.titleAlpha());
    else if(title==TitleKind::Hornet)text.bossTitle("Hornet",game.greenpath.titleAlpha());
    else if(title==TitleKind::Area)text.areaTitle(game.areaTitle);
    if(!game.sceneActive()&&hk3ds::hudEventVisible(overlay))text.dialogue(game.conditionalEvents,actors);
    if(hk3ds::hudEventVisible(overlay))text.cartographer(game.cartographer,game.geo,actors);
    // main.cpp:1026 and :2134: Hornet's four intro lines use the same original box; the arrow stays
    // on pages 0-2 and the stop marker shows on the last page.
    if(hk3ds::hudEventVisible(overlay))text.greenpathIntro(game.greenpath,actors);
    const auto& quest=game.worldQuestDialogue;
    if(questDialogueBoxOpen(game)&&hk3ds::hudEventVisible(overlay)){
        if(quest.choiceVisible)
            text.choiceBox(quest.text(),0,quest.choiceYes,actors,nullptr,true,false,true,true,quest.yesLabel,quest.noLabel);
        else{
            char revealed[768]{};
            const auto count=std::min<std::size_t>(quest.visibleCharacters(),sizeof(revealed)-1);
            std::snprintf(revealed,sizeof(revealed),"%.*s",int(count),quest.text());
            text.dialoguePage(revealed,true,quest.hasNextPage(),actors);
        }
    }
    // Same menu selection as TutorialRenderState::capture (tutorial_render_state.cpp:78). Not behind
    // sceneActive(): the toll sits in loaded scene rooms where sceneActive() is always true.
    if(hk3ds::hudEventVisible(overlay))text.routeToll(game.cityGate.menu.open?game.cityGate.menu:game.routeTolls.menu,game.geo,actors);
    // Stag station toll (HK3DS hud_renderer.cpp drawResting, ChoiceDialog). User 2026-11-21 (PSP photo, Crossroads_47):
    // Up at the toll froze the hero with nothing on screen because the PSP drew no toll dialog.
    if(hk3ds::hudEventVisible(overlay))text.stagToll(game.resting,game.geo,actors);
    if(hk3ds::hudEventVisible(overlay)){
        if(game.resting.stagPhase==hk3ds::StagPhase::Dialogue)text.stagDialogue(game.resting,actors);
        else if(game.resting.stagPhase==hk3ds::StagPhase::Menu)text.stagTravel(game.resting,actors);
    }
}
}
