#pragma once
#include "ui.hpp"
#include "ui_window.hpp"
#include "shop_menu.hpp"
#include "menu_renderer.hpp"
#include "animation.hpp"
#include "hud.hpp"
#include "tutorial_prompts.hpp"
#include "opening_credits.hpp"
#include "lore_tablets.hpp"
#include "area_title.hpp"
#include "charms.hpp"
#include <citro2d.h>

namespace hk3ds {
class PauseMenu;
class InventoryRenderer;
class SimpleMap;
class FuryChest;
class ConditionalEvents;
struct AbilityPickup;
class HeroRenderer;
class Resting;
class WorldQuestDialogue;
class Cartographer;
struct SlyShop;
struct TollMenu;
struct SlyShopArt;
// GPU adapter; all health, soul and geo presentation state stays in hud.hpp.
class HudRenderer {
public:
    void inventoryArtwork(const InventoryRenderer* art){inventoryArt_=art;}
    void turnReadingPage(int direction);
    // Bottom LCD owns reading; the top LCD keeps world and vital information.
    bool readingPanelVisible(const ConditionalEvents*,const WorldQuestDialogue*,const Cartographer*,const AbilityPickup* =nullptr,const char* hornet=nullptr) const;
    // A locked lore inspection is presented on the top screen (lore()), so the lower screen must not repeat it.
    bool loreOnTopScreen() const { return inspection_&&inspection_->locked(); }
    void drawReadingPanel(const ConditionalEvents*,const WorldQuestDialogue*,const Cartographer*,const AbilityPickup* =nullptr,const char* hornet=nullptr) const;
    ReadingSources readingSources(const ConditionalEvents*,const WorldQuestDialogue*,const Cartographer*,const AbilityPickup*,const char* hornet) const;
    // Lower screen shows reading content (choice or Focus) instead of the map.
    bool lowerReadingVisible(const ConditionalEvents*,const WorldQuestDialogue*,const Cartographer*,const AbilityPickup*,const char* hornet) const;
    // Top-screen board (original lore layout) for Hornet, Cornifer/Card and pickup text.
    void drawReadingBoard(C3D_RenderTarget*,const ConditionalEvents*,const WorldQuestDialogue*,const Cartographer*,const AbilityPickup*,const char* hornet) const;
    ui::Canvas uiCanvas() const;
    ui::Canvas menuCanvas() const;
    void observeInspection(const LoreInspection*);
    bool load(const char* directory);
    void release();
    bool ready() const { return pages_[0]&&pages_[1]; }
    void invalidate() { dirty_=true; }
    void advance(float seconds,const HudPresentation& hud);
    void advanceMenu(float seconds);
    void observePrompts(const TutorialPrompts* prompts);
    void drawAreaTitle(const AreaTitle& title) const;
    void drawBossTitle(float alpha) const;
    void drawHornetTitle(float alpha) const;
    void drawCredits(const OpeningCredits& credits) const;
    void drawResting(const Resting&,unsigned geo,bool detail=false) const;
    void drawResting(const Resting&,float stationX,float stationY) const;
    void drawCartographer(const Cartographer&,const AnimationBank&,const HeroRenderer&) const;
    void drawSlyShop(const SlyShop&,unsigned geo,bool details=false) const;
    void drawIseldaShop(const Cartographer&,unsigned geo,bool details=false) const;
    void drawRouteToll(const TollMenu&,unsigned geo,bool details=false) const;
    // Floating door label centered at screen (x,y); y is the backboard center.
    void drawDoorPrompt(const char* label,float x,float y,float alpha=1) const;
    void drawAbility(const AbilityPickup& pickup,const AnimationBank& art,const HeroRenderer& renderer) const;
    void drawCharms(const CharmInventory&,const CharmsPage&,bool canEquip,unsigned geo,const AnimationBank* items=nullptr,const HeroRenderer* renderer=nullptr,bool embedded=false,bool detail=false) const;
    void drawMenu(C3D_RenderTarget* target,const PauseMenu& menu) const;
    void drawMenuDetail(const PauseMenu&,const char* room=nullptr,const HudPresentation* hud=nullptr,const SimpleMap* map=nullptr,Vec2 hero={}) const;
    void drawBottomPanel(C3D_RenderTarget* target) const;
    // Called within the top screen's C3D frame, after its postprocessing.
    void draw(C3D_RenderTarget* target,const HudPresentation& hud,const FuryChest* chest=nullptr,const AnimationBank* items=nullptr,const HeroRenderer* renderer=nullptr,const ConditionalEvents* events=nullptr,const AnimationBank* eventArt=nullptr,const HeroRenderer* eventRenderer=nullptr,const WorldQuestDialogue* quest=nullptr,bool nativeVitals=false);
    std::size_t textureBytes() const;
    unsigned draws() const { return draws_; }
    // Inventory labels use the shared native-LCD body size.
    void inventoryText(const char* value,float x,float baseline,float alpha=1) const {
        text(value,x,baseline,alpha,ui::scale(ui::Size::Body));
    }
private:
    const InventoryRenderer* inventoryArt_=nullptr;
    ShopMenuCanvas shopCanvas() const;
    // Shared top/bottom dialog canvas (ui::choiceDialog, ui::stagStationList/Map): serif text, source menu text, icons.
    ui::DialogCanvas dialogCanvas() const;
    void shopIcon(const char*,ui::Rect) const;
    mutable std::uint64_t readingIdentity_=0;
    mutable unsigned readingPage_=0,readingPages_=1;
    const SpriteFrame* frame(const Clip* clip,float seconds=0,unsigned index=~0u) const;
    const Clip* maskClip(const HudMask& mask) const;
    void sprite(const SpriteFrame* frame,float x,float y,float scale=1,const C2D_ImageTint* tint=nullptr,float verticalScale=1,bool snap=true,float rotation=0) const;
    void textLines(const char*,float,float,float,float,bool) const;
    void glyph(char value,float x,float baseline,float scale,const C2D_ImageTint* tint) const;
    void number(std::uint32_t value,float x,float y,float alpha=1) const;
    void liquid(C3D_RenderTarget* target,const HudPresentation& hud);
    void soulFeedback(const HudPresentation& hud) const;
    void limiterFeedback(const HudPresentation& hud) const;
    void refillFeedback(const HudPresentation& hud) const;
    bool itemVisible_=false,promptVisible_=false;
    float textWidth(const char* value,float scale,float spacing=0) const;
    void text(const char* text,float x,float baseline,float alpha=1,float scale=1.15f,float spacing=0) const;
    void button(const char* label,float x,float y,float alpha) const;
    void prompts(HudOverlay overlay, bool lorePrompt=false) const;
    void lore() const;
    SlyShopArt* shopArt_=nullptr; // Heap-owned: keep the ARM session stack bounded.
    AnimationBank bank_;
    bool eventVisible_=false;
    unsigned eventPage_=~0u;
    std::array<C2D_SpriteSheet,2> pages_{};
    mutable MenuRenderer menuArt_;
    DVLB_s* liquidShader_=nullptr;
    shaderProgram_s liquidProgram_{};
    bool liquidProgramReady_=false;
    const Clip* healthEmpty_=nullptr,*healthIdle_=nullptr,*healthBreak_=nullptr,*healthRefill_=nullptr;
    const Clip* blueIdle_=nullptr,*blueBreak_=nullptr,*blueAppear_=nullptr;
    const Clip* coinIdle_=nullptr,*coinGet_=nullptr,*soulShape_=nullptr,*soulEyes_=nullptr,*frame_=nullptr;
    const Clip* coinAppear_=nullptr,*coinGetOnce_=nullptr;
    const Clip* liquidIdle_=nullptr,*liquidShrink_=nullptr;
    const SpriteFrame* lastLiquidFrame_=nullptr;
    const Clip* plus_=nullptr,*minus_=nullptr;
    const Clip* soulLimiter_=nullptr;
    const Clip* limiterBurst_=nullptr,*limiterFragment_=nullptr;
    const Clip* crackedFrame_=nullptr;
    const Clip* soulFlash_=nullptr,*soulGlow_=nullptr,*soulRim_=nullptr,*soulBurst_=nullptr,*refillParticle_=nullptr;
    std::array<const Clip*,10> digits_{};
    std::array<std::array<const Clip*,95>,3> promptGlyphs_{};
    std::array<const Clip*,sizeof(creditsdata::texts)/sizeof(creditsdata::texts[0])> creditLabels_{};
    std::array<const Clip*,areatitledata::parts> areaTitleParts_{};
    std::array<const Clip*,areatitledata::ornaments> areaTitleOrnaments_{};
    const Clip* promptBackboard_=nullptr,*focusPanel_=nullptr,*promptButton_=nullptr;
    const Clip* loreBackboard_=nullptr;
    const Clip* loreArrow_=nullptr,*loreStop_=nullptr;
    const Clip* loreFleurTop_=nullptr,*loreFleurBottom_=nullptr;
    const Clip* pickupFleur_=nullptr;
    std::array<const Clip*,95> loreGlyphs_{};
    const LoreInspection* inspection_=nullptr;
    unsigned inspectionRevision_=~0u;
    const TutorialPrompts* prompts_=nullptr;
    unsigned promptRevision_=~0u;
    float seconds_=0,menuSeconds_=0;
    unsigned draws_=0;
    bool dirty_=true;
};
}
