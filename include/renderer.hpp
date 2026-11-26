#pragma once
#include "animation.hpp"
#include "actor_texture_budget.hpp"
#include <citro2d.h>
#include <vector>
namespace hk3ds
{
class HeroRenderer
{
public:
    bool load(const char* directory, const AnimationBank& bank, const char* prefix = "page-");
    bool loadBudgeted(const char* root,const AnimationBank& bank,const char* prefix);
    bool loadGuarded(const char* root,const AnimationBank& bank,const char* prefix,
                     const ActorTexturePage* pages,std::size_t pageCount,
                     std::size_t futureBytes=0);
    bool setLinearFilter(unsigned page);
    void drawAnimation(const AnimationPlayer& player, const AnimationBank& bank, float x, float y, float ppu, float scaleX, float scaleY=1,float opacity=1,float rotation=0,float whiteFlash=0) const;
    void release();
    void draw(const Hero& hero, const HeroAnimator& animator, const AnimationBank& bank, float x, float y, float pixelsPerUnit,float whiteFlash=0) const;
    void drawSlash(const Hero& hero, const HeroAnimator& animator, const AnimationBank& bank,
                   const NailBank& nails, float x, float y, float pixelsPerUnit) const;
    bool ready() const { return !pages_.empty(); }
private:
    void drawFrame(const SpriteFrame& frame, float x, float y, float ppu, float scaleX, float scaleY,float opacity=1,float rotation=0,float whiteFlash=0) const;
    std::vector<C2D_SpriteSheet> pages_;
};
}
