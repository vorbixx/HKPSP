#pragma once
#include "lighting_math.hpp"
#include "scene_lighting.hpp"
#include "lantern_glow.hpp"
#include <citro2d.h>
namespace hk3ds
{
class ScreenEffects;
class LightingRenderer
{
public:
    bool load(const char* directory);
    void selectScene(const SceneLighting* scene=nullptr,bool lantern=false) {
        scene_=scene?*scene:SceneLighting{};
        config_=tutorialConfig_;
        heroRgb_={1,1,1};
        if(scene){
            config_.sceneDarkness=scene->darkness;
            config_.transform[3]=scene->heroAlpha;
            heroRgb_=scene->heroRgb;
            scene->configureDarkness(config_);
            const auto scales=sceneDarknessScales(tutorialConfig_,scene->darkness,lantern);
            config_.scales[0]=scales[0];config_.scales[1]=scales[1];
        }
    }
    const SceneLighting& scene() const {return scene_;}
    SceneLightValues sampleScene(float x,float y) {
        const auto values=scene_.sample(x,y);
        config_.transform[3]=values.heroAlpha;heroRgb_=values.heroRgb;
        return values;
    }
    void release();
    bool ready() const {return sheets_[0]&&(sheets_[1]||vignetteInVram_)&&sheets_[2];}
    const LightingConfig& config() const {return config_;}
    void drawHeroLight(Vec2 hero,bool facingRight,float scale,float cameraX,float cameraY,ScreenEffects& effects) const;
    void drawHeroGlow(Vec2 hero,bool facingRight,float cameraX,float cameraY,ScreenEffects& effects) const;
    void drawLanternGlow(Vec2 hero,const DarknessState& state,float cameraX,float cameraY) const;
    void drawVignette(Vec2 hero,bool facingRight,float scale,float cameraX,float cameraY) const;
private:
    void sprite(unsigned index,Vec2 position,float sx,float sy,float depth,float alpha,float cameraX,float cameraY,
                ScreenEffects* effects=nullptr) const;
    std::array<C2D_SpriteSheet,3> sheets_{};
    C3D_Tex vignetteVram_{};
    bool vignetteInVram_=false;
    SceneLighting scene_;
    C2D_SpriteSheet lanternSheet_=nullptr;
    LanternGlowConfig lanternConfig_;
    LightingConfig config_;
    LightingConfig tutorialConfig_;
    std::array<float,3> heroRgb_{1,1,1};
};
}
