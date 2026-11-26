#include "hud_draw.hpp"
#include "hud_data.hpp"
#include "sprite_layout.hpp"
#include "perf.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace hkpsp {
namespace {
namespace d=hk3ds::huddata;
enum Clip { Frame,Cracked,Shape,Eyes,Liquid,Shrink,Limiter,Glow,Rim,Burst,Flash,
    LimiterBurst,Fragment,Particle,Empty,White,Break,Refill,Blue,BlueBreak,BlueAppear,
    Coin,CoinLoop,CoinAppear,CoinGet };
constexpr const char* names[]={"HUD Frame Idle","HUD Frame Cracked","Soul Mask","Soul Eyes",
    "Liquid Idle","Liquid Shrink","Soul Limiter Idle","Soul Full Glow","Soul Full Rim",
    "Soul Burst","Soul Gain Flash","Soul Limiter Burst","Soul Limiter Fragment",
    "Health Refill Particle","Health Empty","Health Idle","Health Break","Health Refill",
    "Blue Idle","Blue Break","Blue Appear","Coin Idle","Coin Get Loop","Coin Appear","Coin Get"};
Quad sprite(const hk3ds::SpriteFrame& s,unsigned page,Point anchor,float scale=1,float sy=1,
            float alpha=1,bool snap=true,float rotation=0,float pixelsPerUnit=hudPixelsPerUnit){
    Quad q;q.page=page+s.page;q.clut=s.clut;q.u0=float(s.x);q.v0=float(s.y);
    q.u1=float(s.x+s.width);q.v1=float(s.y+s.height);q.rgba[3]=alpha;
    const auto layout=hk3ds::spriteLayout(s,anchor.x,anchor.y,pixelsPerUnit,scale,scale*sy);
    float x=layout.x,y=layout.y;
    if(snap){x=std::round(x);y=std::round(y);}
    const float w=s.width*layout.scaleX,h=s.height*layout.scaleY;
    q.positions={{{x,y},{x+w,y},{x+w,y+h},{x,y+h}}};
    if(rotation!=0){
        const auto r=hk3ds::rotatedSpriteLayout(s,anchor.x,anchor.y,pixelsPerUnit,scale,scale*sy,rotation);
        const float c=std::cos(rotation),sn=std::sin(rotation);
        for(auto& p:q.positions){const float dx=p.x-x-w*.5f,dy=p.y-y-h*.5f;
            p={r.x+dx*c+dy*sn,r.y-dx*sn+dy*c};}
    }
    return q;
}
// The same edge-UV/linear-filter convention as the host raster and GE.
float sampleAlpha(const Quad& q,const Texture& t,float x,float y){
    const auto a=q.positions[0],b=q.positions[2];
    if(x<a.x||x>=b.x||y<a.y||y>=b.y)return 0;
    const float u=q.u0+(x-a.x)/(b.x-a.x)*(q.u1-q.u0)-.5f;
    const float v=q.v0+(y-a.y)/(b.y-a.y)*(q.v1-q.v0)-.5f;
    const float fx=std::floor(u),fy=std::floor(v),wx=u-fx,wy=v-fy;
    float alpha=0;
    for(int j=0;j<2;++j)for(int i=0;i<2;++i){
        const auto tx=std::clamp(int(fx)+i,int(q.u0),int(q.u1)-1);
        const auto ty=std::clamp(int(fy)+j,int(q.v0),int(q.v1)-1);
        alpha+=t.paletteFor(q.clut)[t.indices.at(ty*t.width+tx)][3]*(i?wx:1-wx)*(j?wy:1-wy);
    }
    return alpha/255.f;
}
void liquid(DrawList& list,const HudArt& art,const hk3ds::HudPresentation& hud,Point soul,float pixelsPerUnit,HudTextCache::Liquid* cache){
    if(hud.displayedSoul()<=1)return;
    const auto shape=sprite(art.frame(Shape),art.firstPage,soul,1,1,1,true,0,pixelsPerUnit);
    const auto& liquidFrame=art.frame(hud.liquidAnimation()==hk3ds::HudLiquidAnimation::Shrink?Shrink:Liquid,hud.liquidSeconds());
    auto q=sprite(liquidFrame,
        art.firstPage,{soul.x+d::liquidX*pixelsPerUnit,
        soul.y-(d::liquidBottomY+d::liquidYPerSoul*hud.displayedSoul())*pixelsPerUnit},d::liquidScale,1,1,false,0,pixelsPerUnit);
    const int x0=int(std::clamp(std::floor(q.positions[0].x),0.f,float(screenWidth)));
    const int x1=int(std::clamp(std::ceil(q.positions[2].x),0.f,float(screenWidth)));
    const int y0=int(std::clamp(std::floor(q.positions[0].y),0.f,float(screenHeight)));
    const int y1=int(std::clamp(std::ceil(q.positions[2].y),0.f,float(screenHeight)));
    if(x1-x0>128||y1-y0>128)throw std::runtime_error("HUD liquid span bounds exceeded");
    // PSP alpha is stencil, not fragment alpha. Coalesce equal 8-bit effective
    // alpha into horizontal spans. This first pass changes only target alpha;
    // pass two samples moving RGB with DST_ALPHA / ONE_MINUS_DST_ALPHA.
    const std::array<float,16> geometry{{shape.positions[0].x,shape.positions[0].y,shape.positions[2].x,shape.positions[2].y,
        shape.u0,shape.v0,shape.u1,shape.v1,q.positions[0].x,q.positions[0].y,q.positions[2].x,q.positions[2].y,
        q.u0,q.v0,q.u1,q.v1}};
    const std::array<unsigned,5> sampling{{shape.page,shape.clut,q.page,q.clut,unsigned(hud.liquidAnimation())}};
    const bool reuse=cache&&cache->frame==&liquidFrame&&cache->textures==list.textures&&
        cache->soul==hud.displayedSoul()&&cache->geometry==geometry&&cache->sampling==sampling;
    const auto emit=[&](int start,int end,int y,unsigned alpha){
        Quad s;s.blend=Blend::MaskAlpha;s.rgba[3]=alpha/255.f;
        s.positions={{{float(start),float(y)},{float(end),float(y)},
                      {float(end),float(y+1)},{float(start),float(y+1)}}};
        list.quads.push_back(s);
    };
    if(reuse){
        for(const auto& s:cache->spans)emit(s.x0,s.x1,s.y,s.alpha);
    }else{
        // Invalidate before allocation/sampling so a failed rebuild cannot reuse a
        // partial list. Uncached callers remain the independent production oracle.
        if(cache){cache->frame=nullptr;cache->spans.clear();}
        for(int y=y0;y<y1;++y){
            int start=x0;unsigned previous=0;
            const auto span=[&](int end){
                emit(start,end,y,previous);
                if(cache)cache->spans.push_back({std::uint16_t(start),std::uint16_t(end),std::uint16_t(y),std::uint8_t(previous)});
            };
            for(int x=x0;x<=x1;++x){
                const unsigned alpha=x==x1?256:unsigned(std::lround(255*
                    sampleAlpha(shape,list.textures->at(shape.page),x+.5f,y+.5f)*
                    sampleAlpha(q,list.textures->at(q.page),x+.5f,y+.5f)));
                if(x!=x0&&alpha!=previous){span(x);start=x;}
                previous=alpha;
            }
        }
        if(cache){cache->geometry=geometry;cache->sampling=sampling;cache->textures=list.textures;
            cache->soul=hud.displayedSoul();cache->frame=&liquidFrame;++cache->rebuilds;}
    }
    q.blend=Blend::MaskedLiquid;q.rgba={hud.liquidTone(),hud.liquidTone(),hud.liquidTone(),1};
    list.quads.push_back(q);
}
}
HudArt::HudArt(hk3ds::AnimationBank art,FontBank text,unsigned first,unsigned font):
    bank(std::move(art)),fonts(std::move(text)),firstPage(first),fontPage(font){
    for(unsigned i=0;i<clips.size();++i){clips[i]=bank.find(names[i]);
        if(!clips[i])throw std::runtime_error(std::string("Missing HUD clip: ")+names[i]);}
    for(unsigned c='0';c<='9';++c)fonts.glyph(1,c);
    fonts.glyph(1,'+');
}
const hk3ds::SpriteFrame& HudArt::frame(unsigned clip,float seconds,unsigned index)const {
    hk3ds::AnimationPlayer player;player.play(clips.at(clip));
    if(index==~0u)player.advance(seconds);else player.frame=std::min<std::uint32_t>(index,player.clip->count-1);
    const auto* s=player.current(bank);if(!s)throw std::runtime_error("Invalid HUD frame");return *s;
}
void appendHud(DrawList& list,const HudArt& art,const hk3ds::HudPresentation& hud,float seconds,HudTextCache* textCache,bool large){
    constexpr float reducedScale=.85f,largePixelsPerUnit=hudPixelsPerUnit/reducedScale;
    constexpr Point largeAnchor{(hudAnchor.x+2.1f*largePixelsPerUnit)/reducedScale,
                                1+(hudAnchor.y-1+1.2f*largePixelsPerUnit)/reducedScale};
    const float pixelsPerUnit=large?largePixelsPerUnit:hudPixelsPerUnit;
    const Point anchor=large?largeAnchor:hudAnchor;
    const Point soul{anchor.x-d::sourceFrameX*pixelsPerUnit,anchor.y+d::sourceFrameY*pixelsPerUnit};
    const auto draw=[&](unsigned clip,Point at,float clock=0,float scale=1,float sy=1,float alpha=1,bool snap=true,float rotation=0,unsigned index=~0u){
        if(alpha>0&&scale>0)list.quads.push_back(sprite(art.frame(clip,clock,index),art.firstPage,at,scale,sy,alpha,snap,rotation,pixelsPerUnit));
    };
    const auto offset=[&](float x,float y){return Point{anchor.x+x*pixelsPerUnit,anchor.y-y*pixelsPerUnit};};
    const auto maskAt=[&](unsigned slot){return offset(d::sourceMaskX+slot*d::sourceMaskSpacing,d::sourceMaskY);};
    draw(hud.snapshot().soulLimited()?Cracked:Frame,anchor,seconds);
    liquid(list,art,hud,soul,pixelsPerUnit,textCache&&cpu::hudLiquid?&textCache->liquid:nullptr);
    draw(Eyes,{soul.x+d::eyesOffsetX*pixelsPerUnit,soul.y-d::eyesOffsetY*pixelsPerUnit},seconds,1,hud.soulEyesScale(),hud.soulEyesAlpha());
    if(hud.snapshot().soulLimited())draw(Limiter,soul);
    if(hud.soulFull()){draw(Glow,anchor);draw(Rim,anchor,0,1,1,hud.soulRimAlpha());}
    if(hud.soulBurstSeconds()<d::soulBurstSeconds)draw(Burst,anchor,hud.soulBurstSeconds());
    draw(Flash,anchor,0,1,1,hud.soulFlashAlpha());
    if(hud.limiterBurstSeconds()<d::limiterBurstSeconds)draw(LimiterBurst,anchor,hud.limiterBurstSeconds());
    for(const auto& p:hud.limiterFragments())if(p.visible())
        draw(Fragment,offset(p.x,p.y),0,p.currentSize(),1,p.alpha(),false,p.rotation,p.variant);
    if(hud.refillDrops()!=0)throw std::runtime_error("HUD refill capacity exceeded");
    for(const auto& p:hud.refillParticles())if(p.visible()&&p.currentSize()>0){
        const auto at=p.emitter==2?anchor:maskAt(p.mask);
        draw(Particle,{at.x+p.x*pixelsPerUnit,at.y-p.positionY()*pixelsPerUnit},0,p.currentSize());
        list.quads.back().material=hk3ds::RoomMaterial::Additive;
    }
    unsigned slot=0;
    for(const auto& mask:hud.masks()){
        using K=hk3ds::HudMaskKind;using A=hk3ds::HudMaskAnimation;
        if(mask.kind==K::Hidden&&mask.animation==A::None)continue;
        const bool blue=mask.kind==K::Blue||(mask.animation==A::Break&&mask.previous==K::Blue);
        const unsigned clip=mask.animation==A::Break?(blue?BlueBreak:Break):mask.animation==A::Refill?
            (blue?BlueAppear:Refill):mask.kind==K::Empty?Empty:blue?Blue:White;
        draw(clip,maskAt(slot++),mask.animation==A::None?seconds:mask.elapsed,d::sourceMaskScale);
    }
    const auto text=[&](unsigned slot,const char* value,Point center,float alpha){
        auto* entry=textCache?&textCache->entries[slot]:nullptr;
        if(entry&&entry->text==value&&entry->center.x==center.x&&entry->center.y==center.y&&
           entry->alpha==alpha&&entry->fontPage==art.fontPage){
            list.quads.insert(list.quads.end(),entry->quads.begin(),entry->quads.end());return;
        }
        const auto first=list.quads.size();
        // Original MiddleLeft line anchor; final-size Trajan stays at 1:1.
        const auto& f=art.fonts.fonts.at(1);float x=center.x;
        const float baseline=std::round(center.y+(f.ascent-f.descent)*.5f);
        unsigned previous=0;
        for(const auto* p=value;*p;++p){const unsigned cp=static_cast<unsigned char>(*p);
            const auto& g=art.fonts.glyph(1,cp);if(previous)x+=art.fonts.kern(1,previous,cp);
            Quad q;q.page=art.fontPage+g.page;q.u0=float(g.x);q.v0=float(g.y);
            q.u1=float(g.x+g.width);q.v1=float(g.y+g.height);q.rgba[3]=alpha;
            const float px=std::round(x+g.bearingX),py=baseline+g.bearingY;
            q.positions={{{px,py},{px+g.width,py},{px+g.width,py+g.height},{px,py+g.height}}};
            list.quads.push_back(q);x+=g.advance;previous=cp;
        }
        if(entry){
            entry->text=value;entry->center=center;entry->alpha=alpha;entry->fontPage=art.fontPage;
            entry->quads.assign(list.quads.begin()+first,list.quads.end());
        }
    };
    if(hud.geoVisible()){
        const float reveal=hud.geoRevealSeconds();const bool pending=hud.snapshot().geo!=hud.displayedGeo();
        const unsigned coin=reveal<.25f?CoinAppear:reveal<.45f?CoinGet:pending&&hud.geoCountingSeconds()>=0?CoinLoop:Coin;
        draw(coin,offset(d::sourceCoinX,d::sourceCoinY),reveal<.25f?reveal:reveal<.45f?reveal-.25f:std::max(0.f,hud.geoCountingSeconds()));
        char value[12];std::snprintf(value,sizeof value,"%lu",static_cast<unsigned long>(hud.displayedGeo()));
        HKPSP_PHASE(HudText,text(0,value,offset(d::sourceTextX,d::sourceTextY),hud.geoTextAlpha()));
        if(hud.pendingGeoAlpha()>0){
            char pendingText[13];std::snprintf(pendingText,sizeof pendingText,"+ %lu",static_cast<unsigned long>(hud.pendingGeo()));
            HKPSP_PHASE(HudText,text(1,pendingText,offset(d::sourcePendingX,d::sourcePendingY),hud.pendingGeoAlpha()));
        }
    }
}
}
