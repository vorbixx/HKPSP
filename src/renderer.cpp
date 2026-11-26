#include "loading_screen.hpp"
#include "renderer.hpp"
#include <cmath>
#include "combat.hpp"
#include "frame_profile.hpp"
#include "render_memory.hpp"
#include "room_visual.hpp"
#include "sprite_layout.hpp"
#include <cstdio>
#include <cmath>
namespace hk3ds
{
bool HeroRenderer::load(const char* directory, const AnimationBank& bank, const char* prefix)
{
    release();
    for (std::uint32_t i = 0; i < bank.pageCount; ++i)
    {
        char path[256];
        std::snprintf(path, sizeof(path), "%s/%s%lu.t3x", directory, prefix, static_cast<unsigned long>(i));
        FrameProfile::Scope timing(frameProfile,FrameProfile::Page);
        pulseLoadingScreen();
        C2D_SpriteSheet page = C2D_SpriteSheetLoad(path);
        frameProfile.page(page?C2D_SpriteSheetGetImage(page,0).tex->size:0);
        if (!page) { release(); return false; }
        pages_.push_back(page);
    }
    for (const auto& s : bank.sprites)
    {
        if (s.page>=pages_.size()) { release(); return false; }
        const auto image=C2D_SpriteSheetGetImage(pages_[s.page],0);
        if (!image.tex || s.x+s.width>image.tex->width || s.y+s.height>image.tex->height)
        { release(); return false; }
    }
    return ready();
}
bool HeroRenderer::loadBudgeted(const char* root,const AnimationBank& bank,const char* prefix)
{
    release();
    if(!root||!prefix||!bank.pageCount)return false;
    std::vector<ActorTexturePage> plan(bank.pageCount);
    for(unsigned i=0;i<bank.pageCount;++i){
        char path[256];
        std::snprintf(path,sizeof(path),"%s/%s%u.t3x",root,prefix,i);
        auto* file=std::fopen(path,"rb");
        if(!file)return false;
        unsigned char header[4];
        const auto count=std::fread(header,1,sizeof(header),file);
        std::fclose(file);
        if(!actorTexturePageHeader(header,count,plan[i]))return false;
    }
    return loadGuarded(root,bank,prefix,plan.data(),plan.size(),0);
}
bool HeroRenderer::loadGuarded(const char* root,const AnimationBank& bank,const char* prefix,
                               const ActorTexturePage* expectedPages,std::size_t pageCount,
                               std::size_t futureBytes)
{
    release();
    if(!root||!prefix||pageCount!=bank.pageCount)return false;
    std::size_t bankBytes=0;
    const auto fail=[&]{release();return false;};
    if(!actorTexturePlanFits(expectedPages,pageCount,futureBytes,linearSpaceFree(),
                               activeRoomTextureBudget,roomLinearReserve,&bankBytes))return fail();
    pages_.reserve(pageCount);
    std::size_t remainingBytes=bankBytes;
    for(std::size_t i=0;i<pageCount;++i){
        char path[256];
        const auto pathLength=std::snprintf(path,sizeof(path),"%s/%s%lu.t3x",root,prefix,static_cast<unsigned long>(i));
        if(pathLength<0||pathLength>=int(sizeof(path)))return fail();
        FrameProfile::Scope timing(frameProfile,FrameProfile::Page);
        pulseLoadingScreen();
        C2D_SpriteSheet page=C2D_SpriteSheetLoad(path);
        if(!page)return fail();
        const auto image=C2D_SpriteSheetGetImage(page,0);
        frameProfile.page(image.tex?image.tex->size:0);
        if(!image.tex){C2D_SpriteSheetFree(page);return fail();}
        pages_.push_back(page);
        std::size_t expectedBytes=0;
        if(!actorTexturePageBytes(expectedPages[i],expectedBytes)||
           image.tex->width!=expectedPages[i].width||image.tex->height!=expectedPages[i].height||
           image.tex->fmt!=GPU_RGBA4||image.tex->size!=expectedBytes)return fail();
        remainingBytes-=expectedBytes;
        if(!actorTextureBytesFit(remainingBytes,futureBytes,linearSpaceFree(),
                                 activeRoomTextureBudget,roomLinearReserve))return fail();
    }
    for(const auto& s:bank.sprites){
        if(s.page>=pages_.size())return fail();
        const auto image=C2D_SpriteSheetGetImage(pages_[s.page],0);
        if(!image.tex||s.x>image.tex->width||s.y>image.tex->height||
           s.width>image.tex->width-s.x||s.height>image.tex->height-s.y)return fail();
    }
    return ready();
}
void HeroRenderer::drawAnimation(const AnimationPlayer& player,const AnimationBank& bank,float x,float y,float ppu,float scaleX,float scaleY,float opacity,float rotation,float whiteFlash) const
{
    const auto* s=player.current(bank);
    if (s && s->page<pages_.size()) drawFrame(*s,x,y,ppu,scaleX,scaleY,opacity,rotation,whiteFlash);
}
bool HeroRenderer::setLinearFilter(unsigned page)
{
    if(page>=pages_.size())return false;
    const auto image=C2D_SpriteSheetGetImage(pages_[page],0);
    if(!image.tex)return false;
    C3D_TexSetFilter(image.tex,GPU_LINEAR,GPU_LINEAR);return true;
}
void HeroRenderer::release()
{
    for (auto page : pages_) C2D_SpriteSheetFree(page);
    pages_.clear();
}
void HeroRenderer::draw(const Hero& hero, const HeroAnimator& animator, const AnimationBank& bank, float x, float y, float ppu,float whiteFlash) const
{
    const auto* s = animator.player.current(bank);
    if (!s || s->page >= pages_.size()) return;
    // The source faces left. tk2d scale does not scale the physical collider.
    const float sign = hero.facingRight ? -1.0f : 1.0f;
    constexpr float spriteScaleX = 1.0037942f;
    drawFrame(*s,x,y,ppu,sign*spriteScaleX,1,1,0,whiteFlash);
}
void HeroRenderer::drawSlash(const Hero& hero, const HeroAnimator& animator, const AnimationBank& bank,
                             const NailBank& nails, float x, float y, float ppu) const
{
    if (!nails.ready() || animator.slash.complete || !hero.nail.active) return;
    const auto* frame=animator.slash.current(bank);
    if (!frame || frame->page>=pages_.size()) return;
    const auto& shape=nails.shapes[animator.slashVariant];
    const float mirror=hero.facingRight ? -1.0f : 1.0f;
    drawFrame(*frame,x+shape.offset.x*mirror*ppu,y-shape.offset.y*ppu,ppu,
              shape.scale.x*mirror*hero.nail.rangeScale,shape.scale.y*hero.nail.rangeScale);
}
void HeroRenderer::drawFrame(const SpriteFrame& s, float x, float y, float ppu, float scaleX, float scaleY,float opacity,float rotation,float whiteFlash) const
{
    const auto page = C2D_SpriteSheetGetImage(pages_[s.page], 0);
    Tex3DS_SubTexture sub{};
    sub.width = static_cast<u16>(s.width); sub.height = static_cast<u16>(s.height);
    sub.left = static_cast<float>(s.x) / page.tex->width; sub.right = static_cast<float>(s.x + s.width) / page.tex->width;
    sub.top = 1 - static_cast<float>(s.y) / page.tex->height; sub.bottom = 1 - static_cast<float>(s.y + s.height) / page.tex->height;
    const C2D_Image image{page.tex, &sub};
    const auto layout=spriteLayout(s,x,y,ppu,scaleX,scaleY);
    C2D_ImageTint tint;C2D_PlainImageTint(&tint,C2D_Color32f(1,1,1,opacity),whiteFlash);
    if(rotation==0)C2D_DrawImageAt(image,layout.x,layout.y,0,&tint,layout.scaleX,layout.scaleY);
    else{
        const auto rotated=rotatedSpriteLayout(s,x,y,ppu,scaleX,scaleY,rotation);
        const auto quad=centeredSpriteQuad(rotated.scaleX*s.width*(rotated.flipX?-1.f:1.f),
                                           rotated.scaleY*s.height*(rotated.flipY?-1.f:1.f));
        const C2D_DrawParams params{{rotated.x,rotated.y,quad.width,quad.height},
                                   {quad.centerX,quad.centerY},0,-rotation};
        C2D_DrawImage(image,&params,&tint);
    }
}
}
