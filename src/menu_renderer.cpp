#include "text_capture.hpp"
#include "menu_renderer.hpp"
#include "title_font_data.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>

namespace hk3ds {
bool MenuRenderer::load(bool withLogo){
    release();text_=C2D_TextBufNew(2048);
    if(withLogo)logo_=C2D_SpriteSheetLoad("romfs:/title-logo.t3x");
    background_=C2D_SpriteSheetLoad("romfs:/title-background.t3x");
    if(background_)C3D_TexSetFilter(C2D_SpriteSheetGetImage(background_,0).tex,GPU_LINEAR,GPU_LINEAR);
    font_=C2D_SpriteSheetLoad("romfs:/title-font.t3x");
    if(font_)C3D_TexSetFilter(C2D_SpriteSheetGetImage(font_,0).tex,GPU_NEAREST,GPU_NEAREST);
    return text_&&font_&&background_;
}
unsigned MenuRenderer::textureBytes() const {
    unsigned bytes=0;
    for(auto page:{logo_,background_,font_})if(page)bytes+=C2D_SpriteSheetGetImage(page,0).tex->size;
    return bytes;
}
void MenuRenderer::release(){
    for(auto page:{logo_,background_,font_})if(page)C2D_SpriteSheetFree(page);
    logo_=background_=font_=nullptr;
    if(text_)C2D_TextBufDelete(text_);
    text_=nullptr;
}
void MenuRenderer::beginText(){textcapture::select(0,true);if(text_)C2D_TextBufClear(text_);}
void MenuRenderer::arrow(bool right,float x,float y,float width,float height) const {
    if(!font_)return;
    const auto page=C2D_SpriteSheetGetImage(font_,0);
    Tex3DS_SubTexture sub{};sub.width=16;sub.height=16;
    sub.left=right?16.f/page.tex->width:0;sub.right=right?0:16.f/page.tex->width;
    sub.top=1-104.f/page.tex->height;sub.bottom=1-120.f/page.tex->height;
    C2D_DrawImageAt({page.tex,&sub},x,y,0,nullptr,width/16,height/16);
}

void MenuRenderer::item(const char* label,float y,bool selected,float scale){
    const unsigned white=C2D_Color32(235,239,245,255),gray=C2D_Color32(152,163,181,255);
    text(label,200,y,scale,true,selected?white:gray);
    if(selected){
        C2D_DrawTriangle(56,y+7,white,61,y+4,white,61,y+10,white,0);
        C2D_DrawTriangle(344,y+7,white,339,y+4,white,339,y+10,white,0);
    }
}
void MenuRenderer::backdrop(float seconds,bool logo,bool compact){
    C2D_DrawRectSolid(0,0,0,400,240,C2D_Color32(7,12,24,255));
    if(background_){
        auto image=C2D_SpriteSheetGetImage(background_,0);
        C2D_DrawImageAt(image,-3+std::sin(seconds*.06f)*2,-2,0,nullptr,1.02f,1.02f);
    }
    if(font_){
        using namespace hk::menu_font;
        const auto page=C2D_SpriteSheetGetImage(font_,0);
        Tex3DS_SubTexture sub{};sub.width=moteWidth;sub.height=moteHeight;
        sub.left=float(moteX)/page.tex->width;sub.right=float(moteX+moteWidth)/page.tex->width;
        sub.top=1-float(moteY)/page.tex->height;sub.bottom=1-float(moteY+moteHeight)/page.tex->height;
        const auto unit=[](unsigned seed){
            seed^=seed>>16;seed*=0x7feb352du;seed^=seed>>15;seed*=0x846ca68bu;seed^=seed>>16;
            return float(seed&0xffffu)/65535.f;
        };
        // Approximation: project source world units onto the LCD. Fixed phases
        // replace the source 3D box emitter; no allocations or extra textures.
        unsigned index=0;
        for(const auto& emitter:moteEmitters)for(unsigned i=0;i<emitter.count;++i,++index){
            const unsigned seed=(index+1)*2654435761u;
            const float life=emitter.lifeMin+(emitter.lifeMax-emitter.lifeMin)*unit(seed+1);
            const float age=std::fmod(seconds+life*unit(seed+2),life),phase=age/life;
            const float fade=std::min(1.f,std::min(phase/emitter.fadeIn,(1-phase)/(1-emitter.fadeOut)));
            const float vx=emitter.vxMin+(emitter.vxMax-emitter.vxMin)*unit(seed+3);
            const float vz=emitter.vzMin+(emitter.vzMax-emitter.vzMin)*unit(seed+4);
            const float x=std::fmod(unit(seed+5)*400+vx*age*12+440,440.f)-20;
            const float y=std::fmod(unit(seed+6)*240+vz*age*.6f+280,280.f)-20;
            const float size=(emitter.sizeMin+(emitter.sizeMax-emitter.sizeMin)*unit(seed+7))*12;
            C2D_ImageTint tint;C2D_PlainImageTint(&tint,C2D_Color32(emitter.r,emitter.g,emitter.b,unsigned(fade*emitter.a)),1.f);
            C2D_DrawImageAt({page.tex,&sub},x-size/2,y-size/2,0,&tint,size/moteWidth,size/moteHeight);
        }
    }
    if(logo&&logo_)C2D_DrawImageAt(C2D_SpriteSheetGetImage(logo_,0),compact?125:80,compact?8:29,0,nullptr,compact?.5f:.8f,compact?.5f:.8f);
    else if(logo)text("HOLLOW KNIGHT",200,45,.95f);
}
}
