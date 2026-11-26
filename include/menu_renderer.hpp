#pragma once
#include <citro2d.h>
namespace hk3ds {
class HudRenderer;
// Small shared adapter for title and pause/options. Call inside a C3D frame.
class MenuRenderer {
public:
    bool load(bool withLogo=true);
    void release();
    void beginText();
    void text(const char* label,float x,float y,float scale=0.55f,bool centered=true,unsigned color=0xffffffff);
    float textWidth(const char* label,float scale=0.55f) const;
    unsigned textureBytes() const;
    void arrow(bool right,float x,float y,float width=16,float height=16) const;
    void item(const char* label,float y,bool selected,float scale=0.6f);
    void backdrop(float seconds,bool logo=true,bool compact=false);
private:
    C2D_TextBuf text_=nullptr;
    C2D_SpriteSheet logo_=nullptr,background_=nullptr,font_=nullptr;
};
// Returns selected slot (0..3), or -1 on Quit/application close. When supplied,
// newGame is set only for an empty-slot launch; the menu may create a
// placeholder save, but gameplay must still run the authored opening.
int runTitleMenu(C3D_RenderTarget* top,C3D_RenderTarget* bottom,HudRenderer& hud,bool* newGame=nullptr);
}
