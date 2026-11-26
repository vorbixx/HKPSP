#include "lighting.hpp"
#include "screen_effects.hpp"
#include "room_visual.hpp"
#include "frame_profile.hpp"
#include "performance_sections.hpp"
#include "effect_passes.hpp"
#include "render_memory.hpp"
#include <cstdio>
#include <cstring>
namespace hk3ds
{
bool LightingRenderer::load(const char* directory)
{
    release();char path[256];std::snprintf(path,sizeof(path),"%s/lighting.bin",directory);
    if(!config_.load(path))return false;
    tutorialConfig_=config_;
    selectScene();
    const char* names[]={"hero-light","vignette","hero-glow"};
    for(unsigned i=0;i<3;++i){
        std::snprintf(path,sizeof(path),"%s/%s.t3x",directory,names[i]);
        FrameProfile::Scope timing(frameProfile,FrameProfile::Page);
        sheets_[i]=C2D_SpriteSheetLoad(path);
        if(!sheets_[i]){release();return false;}
        const auto im=C2D_SpriteSheetGetImage(sheets_[i],0);frameProfile.page(im.tex?im.tex->size:0);
        unsigned w=8,h=8;while(w<config_.sprites[i].width)w*=2;while(h<config_.sprites[i].height)h*=2;
        if(!im.tex||!im.subtex||(im.tex->fmt!=GPU_RGBA8&&(i!=1||im.tex->fmt!=GPU_LA8))||im.tex->width!=w||im.tex->height!=h){release();return false;}
        C3D_TexSetFilter(im.tex,GPU_LINEAR,GPU_LINEAR);
#if HK3DS_LIGHT_VRAM
        if(i==1&&lightVramFits(vramSpaceFree(),im.tex->size)){
            if(auto* file=std::fopen(path,"rb")){
                auto imported=Tex3DS_TextureImportStdio(file,&vignetteVram_,nullptr,true);
                std::fclose(file);
                if(imported){
                    Tex3DS_TextureFree(imported);
                    if(vignetteVram_.fmt==im.tex->fmt&&vignetteVram_.width==w&&vignetteVram_.height==h&&vignetteVram_.size==im.tex->size){
                        C3D_TexSetFilter(&vignetteVram_,GPU_LINEAR,GPU_LINEAR);
                        C2D_SpriteSheetFree(sheets_[i]);sheets_[i]=nullptr;vignetteInVram_=true;
                    }else C3D_TexDelete(&vignetteVram_);
                }
            }
        }
#endif
    }
    std::snprintf(path,sizeof(path),"%s/lantern-glow.bin",directory);
    if(lanternConfig_.load(path)){
        std::snprintf(path,sizeof(path),"%s/lantern-glow.t3x",directory);
        lanternSheet_=C2D_SpriteSheetLoad(path);
        const auto im=lanternSheet_?C2D_SpriteSheetGetImage(lanternSheet_,0):C2D_Image{};
        if(!im.tex||im.tex->fmt!=GPU_RGBA8||im.tex->width!=512||im.tex->height!=256){
            if(lanternSheet_)C2D_SpriteSheetFree(lanternSheet_);
            lanternSheet_=nullptr;lanternConfig_.duration=0;
        }else{
            C3D_TexSetFilter(im.tex,GPU_LINEAR,GPU_LINEAR);frameProfile.page(unsigned(im.tex->size));
        }
    }
    return true;
}
void LightingRenderer::release(){if(lanternSheet_)C2D_SpriteSheetFree(lanternSheet_);lanternSheet_=nullptr;lanternConfig_.duration=0;for(auto& s:sheets_){if(s)C2D_SpriteSheetFree(s);s=nullptr;}if(vignetteInVram_)C3D_TexDelete(&vignetteVram_);vignetteInVram_=false;}
void LightingRenderer::sprite(unsigned index,Vec2 position,float sx,float sy,float depth,float alpha,float cameraX,float cameraY,
                              ScreenEffects* effects) const
{
    if(effects&&!performanceObservation.on(PerformanceObservation::Lights))return;
    const auto im=index==1&&vignetteInVram_?C2D_Image{const_cast<C3D_Tex*>(&vignetteVram_),nullptr}:C2D_SpriteSheetGetImage(sheets_[index],0);const auto& s=config_.sprites[index];
    // Converter owns atlas padding; its origin does not depend on tex3ds packing.
    Tex3DS_SubTexture sub{};sub.width=s.width;sub.height=s.height;
    sub.left=0;sub.right=float(s.width)/im.tex->width;sub.top=1;sub.bottom=1-float(s.height)/im.tex->height;
    const float ppu=roomFocalLength()/(38.1f+depth);
    const float x=200+(position.x-cameraX+std::min(s.left*sx,s.right*sx))*ppu;
    const float y=120-(position.y-cameraY+s.top*sy)*ppu;
    const float scaleX=(s.right-s.left)*sx*ppu/s.width,scaleY=(s.top-s.bottom)*sy*ppu/s.height;
    if(effects){
        effects->beginLight();
        C2D_SetTintMode(C2D_TintMult);
    }
    const auto rgb=index==0?heroRgb_:std::array<float,3>{1,1,1};
    // SceneManager can supply HDR colors (Greenpath green is 1.007353).
    // Divide the vertex tint, then restore its gain before the UNORM light
    // target clamps it. Clamping the tint itself changes LinearLight.
    const float gain=*std::max_element(rgb.begin(),rgb.end())>1?2.f:1.f;
    if(gain>1){
        auto* e=C3D_GetTexEnv(4);C3D_TexEnvInit(e);
        C3D_TexEnvSrc(e,C3D_Both,GPU_PREVIOUS);
        C3D_TexEnvFunc(e,C3D_Both,GPU_REPLACE);
        C3D_TexEnvScale(e,C3D_RGB,GPU_TEVSCALE_2);
    }
    C2D_ImageTint tint{};C2D_PlainImageTint(&tint,C2D_Color32f(rgb[0]/gain,rgb[1]/gain,rgb[2]/gain,alpha),1);
    C2D_DrawImageAt({im.tex,&sub},x,y,0,&tint,scaleX,scaleY);
    if(effects){
        effects->endLight(index==2?RoomMaterial::VividLight:RoomMaterial::LinearLight);
    }
}
void LightingRenderer::drawHeroLight(Vec2 hero,bool facingRight,float scale,float cameraX,float cameraY,ScreenEffects& effects) const
{
    if(!ready())return;
    hero.y+=config_.transform[1];
    sprite(0,hero,(facingRight?-1:1)*scale,scale,config_.transform[2],config_.transform[3],cameraX,cameraY,&effects);
    C2D_SetTintMode(C2D_TintSolid);
}
void LightingRenderer::drawHeroGlow(Vec2 hero,bool facingRight,float cameraX,float cameraY,ScreenEffects& effects) const
{
    if(!ready())return;
    const auto& t=config_.glowTransform;
    hero.x+=(facingRight?-1:1)*t[2];hero.y+=t[3];
    sprite(2,hero,(facingRight?-1:1)*t[0],t[1],t[4],t[5],cameraX,cameraY,&effects);
    C2D_SetTintMode(C2D_TintSolid);
}
void LightingRenderer::drawLanternGlow(Vec2 hero,const DarknessState& state,float cameraX,float cameraY) const
{
    const float scale=state.lanternGlowScale();
    if(!lanternSheet_||scale<=0)return;
    const auto image=C2D_SpriteSheetGetImage(lanternSheet_,0);
    const auto frame=lanternConfig_.sample(state.lanternGlowAge());
    // Vignette FSM20931 CreateObject: Hero + (0,-0.5,0.006).
    hero.y-=.5f;const float ppu=roomFocalLength()/38.106f;
    C2D_SetTintMode(C2D_TintMult);
    for(unsigned i=0;i<4;++i){
        const auto& s=lanternConfig_.sprites[i==3?1:0];
        Tex3DS_SubTexture sub{};sub.width=s.width;sub.height=s.height;
        sub.left=float(s.x)/512;sub.right=float(s.x+s.width)/512;
        sub.top=1-float(s.y)/256;sub.bottom=1-float(s.y+s.height)/256;
        C2D_ImageTint tint{};C2D_PlainImageTint(&tint,C2D_Color32f(1,1,1,frame[i*4+3]),1);
        C2D_DrawImageAtRotated({image.tex,&sub},200+(hero.x-cameraX)*ppu,120-(hero.y-cameraY)*ppu,0,
            -frame[i*4+2],&tint,s.worldWidth*scale*frame[i*4]*ppu/s.width,
            s.worldHeight*scale*frame[i*4+1]*ppu/s.height);
    }
    C2D_SetTintMode(C2D_TintSolid);
}
void LightingRenderer::drawVignette(Vec2 hero,bool facingRight,float scale,float cameraX,float cameraY) const
{
    if(!ready())return;
    hero.y+=config_.transform[4];const float sx=(facingRight?-1:1)*scale;C2D_SetTintMode(C2D_TintMult);
    sprite(1,hero,sx,scale,config_.transform[5],1,cameraX,cameraY);
    const float ppu=roomFocalLength()/(38.1f+config_.transform[5]);
    for(const auto& r:config_.borders){
        const float a=200+(hero.x-cameraX+r.left*sx)*ppu,b=200+(hero.x-cameraX+r.right*sx)*ppu;
        const float top=120-(hero.y-cameraY+r.top*scale)*ppu,bottom=120-(hero.y-cameraY+r.bottom*scale)*ppu;
        C2D_DrawRectSolid(std::min(a,b),top,0,std::fabs(b-a),bottom-top,C2D_Color32(0,0,0,255));
    }
    C2D_SetTintMode(C2D_TintSolid);
}
}
