#include "text_pack.hpp"
#include "ability_draw.hpp"
#include "sprite_layout.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace hkpsp {
namespace {
// resources.assets Camera11812, verified in the original player at 1920x1080.
constexpr float abilityPixelsPerUnit=270.f/(2*8.710663795471191f);
float ease(float t){
    t=std::clamp(t,0.f,1.f);
    return t<.5f?(1-std::sqrt(std::max(0.f,1-4*t*t)))*.5f:
        (std::sqrt(std::max(0.f,1-4*(1-t)*(1-t)))+1)*.5f;
}
// Native SpriteRenderer alpha channels: sharedassets6 clip87 and resources1138.
float fleurAlpha(float t){
    if(t>=1)return 1;
    if(t<.9333333373f)return std::clamp((8.6096935272f*t-8.0357141495f)*t*t,0.f,1.f);
    t-=.9333333373f;
    return std::clamp(((-1687.5003662109f*t+225.0000305176f)*t+7.5000004768f)*t,0.f,1.f);
}
const char* fleurFrame(float t){
    constexpr float times[]={0,1.0666667223f,1.1333333254f,1.2000000477f,1.2666666508f,
        1.3333333731f,1.3999999762f,1.4666666985f,1.5333333015f,1.8666666746f};
    constexpr const char* names[]={"fleur-0","fleur-1","fleur-2","fleur-3","fleur-4",
        "fleur-5","fleur-6","fleur-7","fleur-8","fleur-9"};
    unsigned frame=0;while(frame+1<10&&t>=times[frame+1])++frame;
    return names[frame];
}
float stopAlpha(float t){
    t-=std::floor(t);
    if(t<.5f)return ((-8*t+4)*t+2)*t;
    t-=.5f;return (8*t-8)*t*t+1;
}
void sprite(DrawList& list,const hk3ds::AnimationBank& art,unsigned page,const char* name,
            Point at,float alpha,bool nativeIcon=false){
    if(alpha<=0)return;
    const auto* clip=art.require(name);
    if(!clip)throw std::runtime_error("Missing original ability panel sprite");
    const auto& s=art.sprites.at(art.frames.at(clip->first));
    const auto r=nativeIcon?hk3ds::SpriteLayout{at.x-s.width*.5f,at.y-s.height*.5f,1,1}:
        hk3ds::spriteLayout(s,at.x,at.y,abilityPixelsPerUnit,1,1);
    // Shared repacked pages (HPT2) need the sprite's own CLUT: without it the black board backdrop used CLUT 0 of
    // another owner and drew white (user PSP photos 2026-11-22, Vengeful Spirit and Mothwing Cloak).
    Quad q;q.page=page+s.page;q.clut=s.clut;q.u0=float(s.x);q.v0=float(s.y);
    q.u1=float(s.x+s.width);q.v1=float(s.y+s.height);q.rgba[3]=alpha;
    const float x=std::round(r.x),y=std::round(r.y),w=s.width*r.scaleX,h=s.height*r.scaleY;
    // The converter crops this oversized source backdrop to the native screen.
    // Its serialized float bounds must not expand that integer crop by an ULP.
    if(!std::strcmp(name,"bg"))q.positions={{{0,0},{480,0},{480,272},{0,272}}};
    else q.positions={{{x,y},{x+w,y},{x+w,y+h},{x,y+h}}};
    list.quads.push_back(q);
}
// The body font's space-Y kern pair (-2.55 px at 9 px) collapsed the space, so
// "Tap Y" drew as "TapY". Spaces keep their full advance; other pairs keep kerning.
float pairKern(const HudArt& art,unsigned font,unsigned left,unsigned right){
    if(left==' '||right==' ')return 0;
    return art.fonts.kern(font,left,right);
}
float width(const HudArt& art,unsigned font,const char* text,std::size_t length){
    float w=0;unsigned previous=0;
    for(std::size_t i=0;i<length;++i){const auto c=static_cast<unsigned char>(text[i]);
        if(previous)w+=pairKern(art,font,previous,c);
        w+=art.fonts.glyph(font,c).advance;previous=c;
    }
    return w;
}
void line(DrawList& list,const HudArt& art,unsigned font,const char* text,std::size_t length,
          float center,float baseline,float alpha){
    if(alpha<=0)return;
    float x=center-width(art,font,text,length)*.5f;unsigned previous=0;
    for(std::size_t i=0;i<length;++i){const auto c=static_cast<unsigned char>(text[i]);
        const auto& g=art.fonts.glyph(font,c);if(previous)x+=pairKern(art,font,previous,c);
        if(g.width&&g.height){Quad q;q.page=art.fontPage+g.page;
            q.u0=float(g.x);q.v0=float(g.y);q.u1=float(g.x+g.width);q.v1=float(g.y+g.height);q.rgba[3]=alpha;
            const float px=std::round(x+g.bearingX),py=std::round(baseline+g.bearingY);
            q.positions={{{px,py},{px+g.width,py},{px+g.width,py+g.height},{px,py+g.height}}};
            list.quads.push_back(q);
        }
        x+=g.advance;previous=c;
    }
}
float text(DrawList& list,const HudArt& art,unsigned font,const char* words,float y,float alpha){
    // Whole-word reflow at the native font size; never shrink to fit the PSP.
    while(*words){
        const auto length=std::strlen(words);std::size_t end=length;
        while(end&&width(art,font,words,end)>420){
            const auto next=end-1;end=next;
            while(end&&words[end]!=' ')--end;
        }
        if(!end)throw std::runtime_error("Unbreakable ability panel word");
        line(list,art,font,words,end,240,y,alpha);
        words+=end;while(*words==' ')++words;
        y+=art.fonts.fonts.at(font).lineHeight;
    }
    return y;
}
}
void appendAbilityFlash(DrawList& list,const hk3ds::AbilityPickup& pickup,
                        const hk3ds::AnimationBank& bank,unsigned page,hk3ds::Vec2 camera){
    const auto flash=pickup.flashDraw(bank,"Pickup Burst");
    if(!flash.visible)return;
    if(!flash.player.clip)throw std::runtime_error("Missing original pickup burst");
    ActorPose pose;pose.position=flash.position;pose.depth=flash.depth;pose.alpha=pickup.flashOpacity();
    appendActor(list,bank,flash.player,pose,camera,page);
}
void appendAbility(DrawList& list,const hk3ds::AbilityPickup& p,
                   const hk3ds::AnimationBank& art,unsigned page,const HudArt& fonts,
                   const hk3ds::AnimationBank* inventory,unsigned inventoryPage){
    using K=hk3ds::AbilityPickup::Kind;using P=hk3ds::AbilityPickup::Phase;
    if(p.trinketSequence()){
        // resources.assets GO4251 Relic Get Msg, Position(-11.91,-6.22).
        // Its child fades are .3s up, .2s down; parent recycles at 5.25s.
        const float t=p.popupAge;
        if(t<0||t>=5.25f)return;
        const float alpha=t<4.25f?ease(t/.3f):1-ease((t-4.25f)/.2f);
        constexpr Point origin{240-11.91f*abilityPixelsPerUnit,136+6.22f*abilityPixelsPerUnit};
        sprite(list,art,page,"trink_bg",{origin.x+2.82f*abilityPixelsPerUnit,origin.y+.12f*abilityPixelsPerUnit},alpha*.7019608021f);
        const Point icon{origin.x-.46f*abilityPixelsPerUnit,origin.y+.05f*abilityPixelsPerUnit};
        if(p.kind==K::CityCrest){
            if(!inventory)throw std::runtime_error("Missing City Crest inventory atlas");
            sprite(list,*inventory,inventoryPage,"CityCrest",icon,alpha,true);
        }
        else sprite(list,art,page,"key",icon,alpha);
        // TMP text is left aligned at the popup origin. Keep capitals >=9px.
        const char* name=p.kind==K::CityCrest?"City Crest":"Simple Key";
        line(list,fonts,0,name,std::strlen(name),origin.x+width(fonts,0,name,std::strlen(name))*.5f,origin.y+5,alpha);
        return;
    }
    if(p.kind==K::Spirit){
        // Vengeful Spirit's Dialogue/Confirm text is the top-screen reading board,
        // which the PSP does not draw yet. Only the granted Item panel is drawn here.
        if(p.phase!=P::Item)return;
        const float top=ease(p.age-.5f),bottom=ease(p.age-2.5f);
        // The original spell board has an opaque black backdrop (the other boards use the .766 veil).
        sprite(list,art,page,"bg",{240,136},ease(p.age));
        sprite(list,art,page,fleurFrame(p.age),{240,136+1.48f*abilityPixelsPerUnit},fleurAlpha(p.age));
        text(list,fonts,0,"Consumed the",126,top);
        text(list,fonts,3,p.title(),147,top);
        // Rows 1-3 are the original Item text (ability_pickup.hpp line()); the
        // 3DS board has no icon, so none is drawn. Row pitch is the body font's.
        const float pitch=fonts.fonts.fonts.at(0).lineHeight;
        for(unsigned row=1;row<=3;++row)text(list,fonts,0,p.line(row),199+(row-1)*pitch,bottom);
        if(p.age>=5.5f)sprite(list,art,page,"stop",{240,263},stopAlpha(p.age-5.5f));
        return;
    }
    if(p.phase==P::MemorialRead||p.phase==P::DreamerRead||p.phase==P::MothRead){
        if(!p.dreamText||p.dreamPage>=p.dreamText->count)return;
        sprite(list,art,page,"bg",{240,136},.766f);
        float baseline=156;
        for(const auto& row:p.dreamText->pages[p.dreamPage].lines){
            const float next=text(list,fonts,0,row.data(),baseline,1);
            baseline=next==baseline?baseline+fonts.fonts.fonts.at(0).lineHeight:next;
        }
        if(p.age>.35f)sprite(list,art,page,"stop",{240,263},stopAlpha(p.age-.35f));
        return;
    }
    if(p.kind==K::DreamNail){
        if(p.phase!=P::Item)return;
        sprite(list,art,page,"bg",{240,136},ease(p.age)*.766f);
        sprite(list,art,page,fleurFrame(p.age),{240,136+1.48f*abilityPixelsPerUnit},fleurAlpha(p.age));
        text(list,fonts,0,"Taken the",126,ease(p.age-.5f));
        text(list,fonts,3,p.title(),147,ease(p.age-.5f));
        if(p.age>=.35f)sprite(list,art,page,"stop",{240,263},stopAlpha(p.age-.35f));
        return;
    }
    if(!p.panelVisible()||(p.kind!=K::Cloak&&p.kind!=K::Claw&&p.kind!=K::CrystalHeart))return;
    const bool closing=p.phase==P::ItemClosing;
    const float fade=closing?1-ease(p.age):1.f;
    const float top=closing?fade:ease(p.age-.5f);
    const float bottom=closing?fade:ease(p.age-2.5f);
    const float second=closing?fade:ease(p.age-3.f);
    const char* icon=p.kind==K::Cloak?"cloak":p.kind==K::Claw?"claw":"heart";
    sprite(list,art,page,"bg",{240,136},(closing?fade:ease(p.age))*.7659999728f);
    sprite(list,art,page,icon,{240,136-4.73f*abilityPixelsPerUnit},top);
    text(list,fonts,0,p.kind==K::CrystalHeart?"Consumed the":"Taken the",126,top);
    text(list,fonts,3,p.title(),147,top);
    sprite(list,art,page,fleurFrame(closing?2.f:p.age),{240,136+1.48f*abilityPixelsPerUnit},
        closing?fade:fleurAlpha(p.age));
    const char* button=p.kind==K::CrystalHeart?"button-l":p.kind==K::Claw?"button-cross":"button-r";
    const char* verb=p.kind==K::CrystalHeart?"HOLD":"Press";
    const auto* buttonClip=art.require(button);
    if(!buttonClip)throw std::runtime_error("Missing original ability button skin");
    const auto& buttonSprite=art.sprites.at(art.frames.at(buttonClip->first));
    const auto buttonRect=hk3ds::spriteLayout(buttonSprite,0,0,abilityPixelsPerUnit,1,1);
    const float buttonWidth=buttonSprite.width*buttonRect.scaleX;
    const float buttonHeight=buttonSprite.height*buttonRect.scaleY;
    const float verbWidth=width(fonts,0,verb,std::strlen(verb)),gap=4;
    const float left=240-(verbWidth+gap+buttonWidth)*.5f;
    line(list,fonts,0,verb,std::strlen(verb),left+verbWidth*.5f,180,bottom);
    sprite(list,art,page,button,{left+verbWidth+gap-buttonRect.x,174-buttonHeight*.5f-buttonRect.y},bottom);
    const char* first=hk3ds::text::resolve(p.kind==K::Cloak?"to dash forwards.":p.kind==K::Claw?
        "@Prompts/GET_WALLJUMP_1":
        "@Prompts/GET_SUPERDASH_1");
    text(list,fonts,0,first,199,bottom);
    const char* secondText=hk3ds::text::resolve(p.kind==K::Cloak?"@Prompts/GET_DASH_2":
        p.kind==K::Claw?"@Prompts/GET_WALLJUMP_2":
        "@Prompts/GET_SUPERDASH_2");
    text(list,fonts,0,secondText,236,second);
    // Reflow body text at the required native font size; reserve the final
    // native row for the original stop decoration below the wrapped text.
    if(closing||p.age>=5.5f)sprite(list,art,page,"stop",{240,263},
        closing?1-ease(p.age/.15f):stopAlpha(p.age-5.5f));
}
}
